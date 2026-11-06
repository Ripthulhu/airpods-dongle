#!/usr/bin/env python3
"""Rebuild the frozen native SCO adapter, then compare every byte and hook."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from check_radio0076 import array, check

ROOT = Path(__file__).resolve().parents[1]
RADIO = ROOT / 'src/radio0076'
NATIVE = RADIO / 'native'
LLVM_REVISION = '9f872551d3c681d06fd303b36f16ed5c274735eb'
SOURCES = ('sco_inbound.c', 'sco_outbound.c', 'native_ring.c', 'bridge_native.c',
           'sco_credit_vendor.c', 'parser_thunk.s', 'thunks.s', 'credit_vendor_thunk.s')


def rebuild(toolchain):
    check()
    clang, linker = toolchain / 'clang', toolchain / 'ld.lld'
    if LLVM_REVISION not in subprocess.check_output([str(clang), '--version'], text=True):
        raise ValueError('Use the pinned Android NDK 30.0.14904198 LLVM compiler')
    if 'LLD 21.0.0' not in subprocess.check_output([str(linker), '--version'], text=True):
        raise ValueError('Use the pinned NDK LLD 21.0.0 linker')
    manifest = json.loads((NATIVE / 'manifest.json').read_text())
    flags = ['--target=arm-none-eabi', '-mcpu=cortex-m3', '-mthumb', '-Os',
             '-ffreestanding', '-fno-builtin', '-fno-unwind-tables',
             '-fno-asynchronous-unwind-tables', '-fstack-usage', '-Wall', '-Wextra', '-Werror']
    with tempfile.TemporaryDirectory(prefix='radio0076-native-') as temp:
        temp = Path(temp)
        objects = []
        for name in SOURCES:
            obj = temp / Path(name).with_suffix('.o')
            objects.append(str(obj))
            subprocess.run([str(clang), *(flags if name.endswith('.c') else flags[:3]),
                            '-c', str(NATIVE / name), '-o', str(obj)], check=True)
        elf = temp / 'bridge.elf'
        subprocess.run([str(linker), '-T', str(NATIVE / 'bridge.ld'), *objects, '-o', str(elf)], check=True)
        data = elf.read_bytes()
        if data[:7] != b'\x7fELF\x01\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 40:
            raise ValueError('Expected ARM32 little-endian ELF')
        offset = struct.unpack_from('<I', data, 32)[0]
        size, count = struct.unpack_from('<HH', data, 46)
        sections = [struct.unpack_from('<10I', data, offset+i*size) for i in range(count)]
        alloc = [s for s in sections if s[2] & 2]
        base = manifest['base']
        end = max(s[3]+s[5] for s in alloc)
        if min(s[3] for s in alloc) != base or end > 0xd8000:
            raise ValueError('Native adapter escaped its reserved RAM')
        blob = bytearray((end-base+3) & ~3)
        for s in alloc:
            if s[1] != 8:  # SHT_NOBITS remains zero-filled, matching the installer.
                blob[s[3]-base:s[3]-base+s[5]] = data[s[4]:s[4]+s[5]]
        symbols = {}
        for s in sections:
            if s[1] != 2:
                continue
            strings = sections[s[6]]
            strings = data[strings[4]:strings[4]+strings[5]]
            for at in range(s[4], s[4]+s[5], s[9]):
                name, value, _, _, _, index = struct.unpack_from('<IIIBBH', data, at)
                if not name:
                    continue
                name = strings[name:strings.index(b'\0', name)].decode()
                if index == 0:
                    raise ValueError('Unresolved native symbol: '+name)
                symbols[name] = value
        if len(blob) != manifest['length'] or hashlib.sha256(blob).hexdigest() != manifest['sha256']:
            raise ValueError('Rebuilt native adapter differs from the qualified binary')
        if blob != array('shared_bridge_blob', RADIO / 'shared_bridge_blob.h'):
            raise ValueError('Rebuilt native adapter differs from the committed header')
        hooks = []
        for hook in manifest['hooks']:
            target = symbols[hook['name']] & ~1
            delta = target-hook['address']-4
            if delta & 1 or not -(1 << 24) <= delta < (1 << 24):
                raise ValueError('Hook branch outside Thumb range')
            v = delta & 0x1ffffff
            sign = v >> 24
            patch = struct.pack('<HH', 0xf000 | (sign << 10) | (v >> 12 & 0x3ff),
                0x9000 | ((1 ^ (v >> 23 & 1) ^ sign) << 13) |
                ((1 ^ (v >> 22 & 1) ^ sign) << 11) | (v >> 1 & 0x7ff))
            if target != hook['target'] or int.from_bytes(patch, 'little') != hook['patch']:
                raise ValueError('Rebuilt hook moved: '+hook['name'])
            hooks.append([hook[k] for k in ('address', 'expected', 'slot', 'patch')])
        header = (RADIO / 'shared_bridge_blob.h').read_text()
        actual = [[int(x, 16) for x in re.findall(r'0x([0-9a-f]+)', row)]
                  for row in header.split('shared_hooks[]={', 1)[1].split('};', 1)[0].splitlines() if '{' in row]
        if hooks != actual:
            raise ValueError('Committed hooks differ from the rebuild')
        for name in ('sco_inbound_stats', 'sco_outbound_state', 'sco_bridge_state', 'sco_credit_vendor_stats'):
            expected = re.search(r'#define '+name.upper()+r'_ADDRESS (0x[0-9a-f]+)u', header)
            if expected is None or symbols[name] != int(expected[1], 16):
                raise ValueError('Native state symbol moved: '+name)
    print('Native rebuild: 4788 bytes, SHA256 '+manifest['sha256']+'; all four hooks and state addresses identical')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path, required=True, help='Pinned NDK LLVM bin directory')
    rebuild(parser.parse_args().toolchain.resolve())
