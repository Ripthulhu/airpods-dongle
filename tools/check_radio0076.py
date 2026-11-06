#!/usr/bin/env python3
"""Verify frozen .0076 inputs; optionally prove the linked override, not an SDK fallback."""
import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
RADIO = ROOT / "src/radio0076"


def array(name, path):
    source = path.read_text()
    body = re.search(r"\b" + name + r"\s*\[\s*\][^=;]*=\s*\{([^}]*)\}", source, re.S)
    if body is None:
        raise ValueError(f"Array missing: {name}")
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", body[1]))


def linked_bytes(data, address, length):
    if data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("Expected 32-bit little-endian ELF")
    offset = struct.unpack_from("<I", data, 28)[0]
    size, count = struct.unpack_from("<HH", data, 42)
    for i in range(count):
        kind, file_offset, va, _, file_size, *_ = struct.unpack_from("<8I", data, offset+i*size)
        if kind == 1 and va <= address and address+length <= va+file_size:
            return data[file_offset+address-va:file_offset+address-va+length]
    raise ValueError("Symbol does not fit a file-backed LOAD segment")


def check(elf=None, nm="arm-none-eabi-nm"):
    for name, digest in json.loads((RADIO/"frozen.json").read_text()).items():
        if hashlib.sha256((RADIO/name).read_bytes()).hexdigest() != digest:
            raise ValueError(f"Frozen .0076 input changed: {name}")
    firmware = array("cyw43_btfw_43439", RADIO/"cyw43_btfw_43439.h")
    bridge = array("shared_bridge_blob", RADIO/"shared_bridge_blob.h")
    if len(firmware) != 24936 or hashlib.sha256(firmware).hexdigest() != "34382f9453009e485ebedede2ec1e7b79d8f868d75829997932175e3aa7b137c":
        raise ValueError("Unexpected .0076 container")
    if len(bridge) != 4788 or hashlib.sha256(bridge).hexdigest() != "c63e482e377d295ff3bb822345a5ac9cf883094d9671563f5f1ad3211394e54e":
        raise ValueError("Unexpected native SCO adapter")
    if elf:
        symbols = {}
        for line in subprocess.check_output([nm,"-S",str(elf)],text=True).splitlines():
            fields=line.split()
            if len(fields)==4:
                symbols[fields[3]]=(int(fields[0],16),int(fields[1],16))
        data=elf.read_bytes()
        for name, expected in [("cyw43_btfw_43439",firmware),("shared_bridge_blob",bridge)]:
            address,size=symbols[name]
            if size != len(expected) or linked_bytes(data,address,size)!=expected:
                raise ValueError(f"Linked .0076 input mismatch: {name}")
        for i in range(5):
            name=f"expected_{i}"
            expected=array(name,RADIO/"expected.h")
            address,size=symbols[name]
            if size != len(expected) or linked_bytes(data,address,size)!=expected:
                raise ValueError(f"Linked readback span mismatch: {name}")
        for name in ("__wrap_cybt_fw_download","__wrap_cybt_set_host_ready","__wrap_cyw43_ll_write_backplane_mem",
                     "radio0076_start","radio0076_after_hci"):
            if name not in symbols:
                raise ValueError(f"Startup guard was omitted: {name}")
        for name in ("fatal_marker_capture","fatal_marker_install","ring_probe_capture","domain_capture"):
            if name in symbols:
                raise ValueError(f"Test instrumentation linked: {name}")
    print("Frozen .0076 container/native adapter verified" + (" in linked ELF" if elf else ""))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf",type=pathlib.Path)
    parser.add_argument("--nm",default="arm-none-eabi-nm")
    args=parser.parse_args()
    check(args.elf,args.nm)
