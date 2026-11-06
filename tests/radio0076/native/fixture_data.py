"""Frozen code-only ROM spans and the repository's exact .0076 signed overlay."""
from pathlib import Path
import hashlib
import json
import sys
import unicorn

ROOT = Path(sys.argv[2])
sys.path.insert(0, str(ROOT/'tools'))
from check_radio0076 import array

ROM_FIXTURE_SHA256 = '0ebadf24b6b58d6331d006a16ff0fbaf83f9b58e4a47a3f6e47ab69da85fe125'
allowed = []


def load_rom():
    data = (ROOT/'tests/radio0076/native/rom-fixture.json').read_bytes()
    if hashlib.sha256(data).hexdigest() != ROM_FIXTURE_SHA256:
        raise ValueError('Frozen ROM test fixture changed')
    fixture = json.loads(data)
    rom = bytearray(0x90000)
    previous = 0
    for row in fixture['spans']:
        address = row['address']; code = bytes.fromhex(row['hex'])
        if not code or address < previous or address+len(code) > len(rom):
            raise ValueError('Invalid ROM fixture span')
        previous = address+len(code)
        rom[address:previous] = code
        allowed.append((address, previous))
    return bytes(rom)


def load_vendor():
    code = array('expected_0', ROOT/'src/radio0076/expected.h')
    if hashlib.sha256(code).hexdigest() != '9fbeb2169dbdacd73ace84937658e282b03794301fc5d6c6a02eb7bd6a1fd701':
        raise ValueError('Signed .0076 test overlay changed')
    return code


def production_adapter():
    manifest = json.loads((ROOT/'src/radio0076/native/manifest.json').read_text())
    blob = array('shared_bridge_blob', ROOT/'src/radio0076/shared_bridge_blob.h')
    if hashlib.sha256(blob).hexdigest() != manifest['sha256']:
        raise ValueError('Native adapter manifest differs from the frozen blob')
    return manifest, blob


_Uc = unicorn.Uc


def checked_uc(*args, **kwargs):
    machine = _Uc(*args, **kwargs)
    def guarded(uc, address, size, *unused):
        if address >= 0x90000 or 0x88000 <= address < 0x88040 or 0x88f00 <= address < 0x88f04:
            return
        if not any(start <= address and address+size <= end for start, end in allowed):
            raise AssertionError('ROM access outside the frozen fixture: '+hex(address))
    start = machine.emu_start
    armed = False
    def emu_start(*args, **kwargs):
        nonlocal armed
        if not armed:
            machine.hook_add(unicorn.UC_HOOK_CODE, guarded)
            machine.hook_add(unicorn.UC_HOOK_MEM_READ, lambda uc, access, address, size, value, user: guarded(uc, address, size))
            armed = True
        return start(*args, **kwargs)
    machine.emu_start = emu_start
    return machine


unicorn.Uc = checked_uc
