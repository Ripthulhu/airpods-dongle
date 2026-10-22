#!/usr/bin/env python3
"""Check the linked firmware's codec heap in ARM emulation, with a runtime reserve."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys


RUNTIME_RESERVE_BYTES = 64 * 1024
RAM_START, RAM_END = 0x20000000, 0x20080000


def firmware_heap(header, symbols):
    """Only the pinned RP2350 ARM32 executable layout has this heap contract."""
    if len(header) < 52 or header[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("Expected a little-endian ELF32 firmware executable")
    kind, machine, version = struct.unpack_from("<HHI", header, 16)
    if (kind, machine, version) != (2, 40, 1):
        raise ValueError("Expected an ARM firmware executable")
    found = {}
    for line in symbols.splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]+)\s+[A-Za-z]\s+(end|__StackLimit)", line)
        if match:
            name = match[2]
            if name in found:
                raise ValueError(f"Duplicate firmware heap symbol: {name}")
            found[name] = int(match[1], 16)
    if set(found) != {"end", "__StackLimit"}:
        raise ValueError("Firmware must define end and __StackLimit")
    start, limit = found["end"], found["__StackLimit"]
    if not RAM_START <= start < limit <= RAM_END or (start | limit) & 7:
        raise ValueError("Firmware heap is outside the supported aligned RP2350 RAM layout")
    capacity = limit - start
    if capacity <= RUNTIME_RESERVE_BYTES:
        raise ValueError("Firmware heap cannot retain the 64 KiB runtime reserve")
    return capacity


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_library(firmware, codec_sha256):
    stamps = set(re.findall(rb"APDG_FDK_SHA256:([0-9a-f]{64})\x00", firmware))
    if stamps != {codec_sha256.encode("ascii")}:
        raise ValueError("Codec archive does not match the fingerprint linked into this firmware")


def run(args):
    directory = pathlib.Path(__file__).resolve().parent
    root = directory.parents[1]
    output = (args.output or root / "build" / "codec-probe").resolve()
    output.mkdir(parents=True, exist_ok=True)
    receipt = output / "memory-check.json"
    # A failed or interrupted recheck cannot leave a stale passing receipt.
    receipt.unlink(missing_ok=True)
    firmware, library = args.firmware.resolve(), args.library.resolve()
    if not firmware.is_file() or not library.is_file():
        raise ValueError("The built firmware ELF and codec archive are required")
    compiler = shutil.which(args.compiler)
    nm = shutil.which(args.nm)
    emulator = shutil.which("qemu-system-arm")
    if compiler is None or nm is None or emulator is None:
        raise ValueError("The ARM compiler, matching nm and qemu-system-arm are required")
    before = {"firmware_sha256": digest(firmware), "codec_sha256": digest(library)}
    verify_library(firmware.read_bytes(), before["codec_sha256"])
    symbols = subprocess.run([nm, "--defined-only", str(firmware)], capture_output=True,
                             text=True, timeout=10, check=True).stdout
    capacity = firmware_heap(firmware.read_bytes()[:52], symbols)
    budget = capacity - RUNTIME_RESERVE_BYTES
    elf = output / "codec-probe.elf"
    fdk = root / "3rd-party" / "fdk-aac"
    command = [compiler, "-mcpu=cortex-m33", "-mthumb", "-march=armv8-m.main+fp+dsp",
               "-mfloat-abi=softfp", "-mcmse", "-O2", "-g", "-ffreestanding",
               "-Wall", "-Wextra", "-Werror", "-DFDK_AIRPODS_PROFILE=1",
               f"-DPROBE_HEAP_BYTES={budget}"]
    for module in ("libAACenc", "libAACdec", "libSYS"):
        command += ["-isystem", str(fdk / module / "include")]
    command += ["-nostartfiles", f"-Wl,-T,{directory / 'probe.ld'}",
                str(directory / "startup.c"), str(directory / "probe.c"), str(library),
                "-Wl,--start-group", "-lc", "-lm", "-lnosys", "-Wl,--end-group",
                "-o", str(elf)]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (output / "build.log").write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise ValueError("Codec probe compilation failed:\n" + compiled.stdout + compiled.stderr)
    result = subprocess.run([emulator, "-M", "mps2-an500", "-nographic",
                             "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(elf)],
                            capture_output=True, text=True, timeout=60)
    transcript = result.stdout + result.stderr
    (output / "result.log").write_text(transcript)
    if result.returncode:
        raise ValueError("Codec memory check failed:\n" + transcript)
    if "SUCCESS frames=16 checksum=4105af5d" not in transcript or "CHECKS complete" not in transcript:
        raise ValueError("Codec probe did not complete every required check")
    peak = re.search(r"^HEAP_PEAK bytes=(\d+)$", transcript, re.MULTILINE)
    if peak is None or not 0 < int(peak[1]) <= budget:
        raise ValueError("Codec probe returned an invalid heap high-water mark")
    if before != {"firmware_sha256": digest(firmware), "codec_sha256": digest(library)}:
        raise ValueError("Firmware or codec archive changed during validation")
    summary = {**before, "heap_capacity_bytes": capacity,
               "runtime_reserve_bytes": RUNTIME_RESERVE_BYTES, "probe_budget_bytes": budget,
               "peak_codec_heap_break_bytes": int(peak[1]),
               "remaining_after_peak_bytes": capacity - int(peak[1]),
               "passed": True, "hardware_tested": False}
    receipt.write_text(json.dumps(summary, indent=2) + "\n")
    print(transcript, end="")
    print(f"Codec memory check passed: {capacity - int(peak[1])} bytes remain at the emulated peak; "
          f"{RUNTIME_RESERVE_BYTES} bytes were reserved for other runtime use.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", required=True, type=pathlib.Path,
                        help="exact linked airpods_dongle.elf")
    parser.add_argument("--library", required=True, type=pathlib.Path,
                        help="codec archive linked into that firmware")
    parser.add_argument("--compiler", default="arm-none-eabi-gcc")
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        run(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"Codec validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
