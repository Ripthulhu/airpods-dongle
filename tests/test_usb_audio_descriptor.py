import importlib.util
import pathlib
import struct
import unittest
from unittest.mock import patch

path = pathlib.Path(__file__).resolve().parents[1] / 'tools/check_usb_audio.py'
spec = importlib.util.spec_from_file_location('check_usb_audio', path)
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


def configuration(floating=True):
    result = bytearray((9, 2, 0, 0, 6, 1, 0, 0x80, 50))
    for itf in range(6):
        result.extend((9, 4, itf, 0, 0, 1, 0, 0x20, 0))
        if itf not in (1, 2):
            continue
        for alt in range(1, (3 if floating and itf == 1 else 2)):
            kind = 4 if alt == 2 else 1
            channels = 2 if itf == 1 else 1
            slot = 4 if alt == 2 else 2
            packet = 392 if alt == 2 else 196 if itf == 1 else 130
            result.extend((9, 4, itf, alt, 1, 1, 2, 0x20, 0))
            general = bytearray((16, 0x24, 1, 1, 0, 1))
            general.extend(struct.pack('<I', kind))
            general.extend((channels, 0, 0, 0, 0, 0))
            result.extend(general)
            result.extend((6, 0x24, 2, 1, slot, slot*8))
            result.extend((7, 5, 1 if itf == 1 else 0x81, 9, packet & 255, packet >> 8, 1))
    struct.pack_into('<H', result, 2, len(result))
    return result


def control_configuration():
    result = bytearray((9, 2, 0, 0, 3, 1, 0, 0x80, 50))
    result.extend((8, 11, 0, 2, 2, 2, 0, 6))
    result.extend((9, 4, 0, 0, 1, 2, 2, 0, 6))
    result.extend((5, 0x24, 0, 0x20, 1, 5, 0x24, 1, 0, 1))
    result.extend((4, 0x24, 2, 2, 5, 0x24, 6, 0, 1))
    result.extend((7, 5, 0x83, 3, 8, 0, 16))
    result.extend((9, 4, 1, 0, 2, 10, 0, 0, 0))
    result.extend((7, 5, 4, 2, 64, 0, 0, 7, 5, 0x84, 2, 64, 0, 0))
    result.extend((9, 4, 2, 0, 1, 3, 0, 0, 7))
    result.extend((9, 0x21, 0x11, 1, 0, 1, 0x22, 80, 0))
    result.extend((7, 5, 0x85, 3, 16, 0, 5))
    struct.pack_into('<H', result, 2, len(result))
    return result


class UsbAudioDescriptorTests(unittest.TestCase):
    def test_control_only_has_no_audio_and_contiguous_interfaces(self):
        check.validate_control(control_configuration())
        for needle, offset, replacement in (
            (bytes((9, 4, 2, 0, 1, 3)), 2, 5),  # Sparse old HID interface number.
            (bytes((9, 4, 2, 0, 1, 3)), 5, 1),  # Accidental audio interface.
            (bytes((7, 5, 0x85, 3)), 2, 0x83),  # Colliding CDC/HID endpoints.
            (bytes((5, 0x24, 6, 0, 1)), 4, 4),  # Old CDC union references.
        ):
            data = control_configuration()
            data[data.index(needle) + offset] = replacement
            with self.assertRaises(ValueError):
                check.validate_control(data)
        with self.assertRaises(ValueError):
            check.validate_control(control_configuration()[:-1])

    def test_linked_check_requires_both_personalities(self):
        device = bytearray(struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x200, 0xef, 2, 1, 64,
                                       0xcafe, 0x4015, 0x100, 1, 2, 3, 1))
        control = bytearray(device)
        struct.pack_into('<H', control, 10, 0x4005)
        symbols = dict(desc_device=device, desc_device_control=control,
                       desc_configuration=configuration(), desc_configuration_control=control_configuration())
        with patch.object(check, 'elf_symbol', side_effect=lambda _, name: symbols[name]):
            self.assertIn('1:2', check.validate_firmware(b'ELF'))
            symbols['desc_configuration'] = configuration(False)
            self.assertNotIn('1:2', check.validate_firmware(b'ELF', False))
            control[16] = 0  # Dropping the serial would lose selected-device identity.
            with self.assertRaises(ValueError):
                check.validate_firmware(b'ELF', False)
            control[16] = 3
            struct.pack_into('<H', control, 10, 0x4015)
            with self.assertRaises(ValueError):
                check.validate_firmware(b'ELF', False)

    def test_formats_and_compatibility_build(self):
        formats = check.validate(configuration())
        self.assertEqual(formats['1:2']['format'], 4)
        self.assertEqual(formats['1:2']['packet_bytes'], 392)
        self.assertEqual(formats['2:1']['bits'], 16)
        self.assertNotIn('1:2', check.validate(configuration(False), False))

    def test_wrong_contract_and_lengths(self):
        for data in (configuration()[:-1], configuration(False), bytes(9)):
            with self.assertRaises(ValueError):
                check.validate(data)
        data = configuration()
        at = data.index(bytes((6, 0x24, 2, 1, 4, 32)))
        data[at + 5] = 24
        with self.assertRaises(ValueError):
            check.validate(data)

    def test_refuses_non_arm_elf(self):
        with self.assertRaises(ValueError):
            check.elf_symbol(b'not a firmware image', 'desc_configuration')
