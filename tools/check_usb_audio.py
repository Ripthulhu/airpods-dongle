#!/usr/bin/env python3
"""Validate USB audio formats in the actual linked ARM firmware descriptor."""
import argparse
import json
import pathlib
import struct


def elf_symbol(data, name):
    if data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Expected ELF32 little-endian firmware')
    offset = struct.unpack_from('<I', data, 32)[0]
    entry_size, count = struct.unpack_from('<HH', data, 46)
    if entry_size != 40 or offset + count*40 > len(data):
        raise ValueError('Invalid ELF section table')
    sections = [struct.unpack_from('<10I', data, offset + i*40) for i in range(count)]
    found = []
    for section in sections:
        if section[1] != 2:
            continue
        if section[9] != 16 or section[6] >= count:
            raise ValueError('Invalid ELF symbol table')
        strings = sections[section[6]]
        table = data[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], 16):
            label, address, size, _, _, index = struct.unpack_from('<IIIBBH', data, at)
            if table[label:].split(b'\0', 1)[0] != name.encode():
                continue
            if index >= count:
                raise ValueError('Invalid descriptor section')
            target = sections[index]
            relative = address - target[3]
            if relative < 0 or relative + size > target[5]:
                raise ValueError('Descriptor exceeds its ELF section')
            start = target[4] + relative
            found.append(data[start:start + size])
    if len(found) != 1:
        raise ValueError('Expected one linked descriptor symbol')
    return found[0]


def validate(data, float_enabled=True):
    if len(data) < 9 or data[:2] != b'\x09\x02' or struct.unpack_from('<H', data, 2)[0] != len(data):
        raise ValueError('Incorrect configuration descriptor length')
    formats, interfaces = {}, set()
    current = None
    offset = 0
    while offset < len(data):
        size, kind = data[offset:offset + 2]
        if size < 2 or offset + size > len(data):
            raise ValueError('Invalid USB descriptor boundary')
        item = data[offset:offset + size]
        if kind == 4:
            if size != 9:
                raise ValueError('Invalid interface descriptor')
            current = (item[2], item[3])
            interfaces.add(item[2])
            if current[0] in (1, 2) and current[1]:
                if current in formats:
                    raise ValueError('Duplicate audio alternate setting')
                formats[current] = {}
        elif current in formats:
            value = formats[current]
            if kind == 0x24 and item[2] == 1:
                value['format'] = struct.unpack_from('<I', item, 6)[0]
                value['channels'] = item[10]
            elif kind == 0x24 and item[2] == 2:
                value['subslot'], value['bits'] = item[4:6]
            elif kind == 5:
                value['endpoint'] = item[2]
                value['packet_bytes'] = struct.unpack_from('<H', item, 4)[0]
                value['interval'] = item[6]
        offset += size
    expected = {
        (1, 1): dict(format=1, channels=2, subslot=2, bits=16, endpoint=1, packet_bytes=196, interval=1),
        (2, 1): dict(format=1, channels=1, subslot=2, bits=16, endpoint=0x81, packet_bytes=130, interval=1),
    }
    if float_enabled:
        expected[(1, 2)] = dict(format=4, channels=2, subslot=4, bits=32, endpoint=1, packet_bytes=392, interval=1)
        expected[(1, 3)] = dict(format=1, channels=2, subslot=4, bits=32, endpoint=1, packet_bytes=392, interval=1)
    if formats != expected or interfaces != set(range(6)) or data[4] != 6:
        raise ValueError('Linked USB interfaces or audio alternatives do not match their contract: %r' % formats)
    return {f'{itf}:{alt}': value for (itf, alt), value in formats.items()}


def validate_control(data):
    if len(data) < 9 or data[:2] != b'\x09\x02' or struct.unpack_from('<H', data, 2)[0] != len(data) or data[4:6] != b'\x03\x01':
        raise ValueError('Incorrect control-only configuration header')
    interfaces, endpoints, associations, unions, call_interfaces = {}, {}, [], [], []
    current, hid_reports, offset = None, 0, 9
    while offset < len(data):
        if len(data) - offset < 2:
            raise ValueError('Truncated control-only descriptor')
        size, kind = data[offset:offset + 2]
        if size < 2 or offset + size > len(data):
            raise ValueError('Invalid control-only descriptor boundary')
        item = data[offset:offset + size]
        if kind == 4:
            if size != 9 or item[3] != 0 or item[2] in interfaces:
                raise ValueError('Invalid control-only interface')
            current = item[2]
            interfaces[current] = tuple(item[4:8])
            endpoints[current] = []
        elif kind == 5:
            if size != 7 or current not in endpoints:
                raise ValueError('Invalid control-only endpoint')
            endpoints[current].append((item[2], item[3], struct.unpack_from('<H', item, 4)[0], item[6]))
        elif kind == 11:
            if size != 8:
                raise ValueError('Invalid control-only association')
            associations.append(tuple(item[2:7]))
        elif kind == 0x24:
            if current != 0 or size < 3:
                raise ValueError('Unexpected control-only class descriptor')
            if item[2] == 6:
                unions.append(tuple(item[3:]))
            elif item[2] == 1:
                if size != 5:
                    raise ValueError('Invalid CDC call management descriptor')
                call_interfaces.append(item[4])
            elif item[2] not in (0, 2):
                raise ValueError('Unexpected CDC functional descriptor')
        elif kind == 0x21:
            if current != 2 or size != 9 or item[5:7] != b'\x01\x22' or struct.unpack_from('<H', item, 7)[0] == 0:
                raise ValueError('Invalid control-only HID report descriptor')
            hid_reports += 1
        else:
            raise ValueError('Unexpected control-only descriptor')
        offset += size
    if (interfaces != {0: (1, 2, 2, 0), 1: (2, 10, 0, 0), 2: (1, 3, 0, 0)} or
        endpoints != {0: [(0x83, 3, 8, 16)], 1: [(4, 2, 64, 0), (0x84, 2, 64, 0)], 2: [(0x85, 3, 16, 5)]} or
        associations != [(0, 2, 2, 2, 0)] or unions != [(0, 1)] or call_interfaces != [1] or hid_reports != 1):
        raise ValueError('Control-only CDC/HID interface contract mismatch')


def validate_firmware(data, float_enabled=True):
    devices = [elf_symbol(data, name) for name in ('desc_device', 'desc_device_control')]
    for descriptor, pid in zip(devices, (0x4015, 0x4005)):
        if (len(descriptor) != 18 or descriptor[:2] != b'\x12\x01' or
            struct.unpack_from('<HH', descriptor, 8) != (0xcafe, pid) or
            descriptor[14:18] != b'\x01\x02\x03\x01'):
            raise ValueError('Incorrect USB device identity')
    if devices[0][:10] + devices[0][12:] != devices[1][:10] + devices[1][12:]:
        raise ValueError('USB personalities must preserve device fields other than product ID')
    validate_control(elf_symbol(data, 'desc_configuration_control'))
    return validate(elf_symbol(data, 'desc_configuration'), float_enabled)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=pathlib.Path)
    parser.add_argument('--s16-only', action='store_true')
    args = parser.parse_args()
    print(json.dumps(validate_firmware(args.firmware.read_bytes(), not args.s16_only), indent=2))


if __name__ == '__main__':
    main()
