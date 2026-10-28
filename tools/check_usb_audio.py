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
    if formats != expected or interfaces != set(range(6)) or data[4] != 6:
        raise ValueError('Linked USB interfaces or audio alternatives do not match their contract: %r' % formats)
    return {f'{itf}:{alt}': value for (itf, alt), value in formats.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=pathlib.Path)
    parser.add_argument('--s16-only', action='store_true')
    args = parser.parse_args()
    print(json.dumps(validate(elf_symbol(args.firmware.read_bytes(), 'desc_configuration'), not args.s16_only), indent=2))


if __name__ == '__main__':
    main()
