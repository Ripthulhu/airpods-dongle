import importlib.util
import pathlib
import struct
import unittest

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


class UsbAudioDescriptorTests(unittest.TestCase):
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
