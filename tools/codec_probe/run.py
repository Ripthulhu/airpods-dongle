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
CODEC_RAM_FUNCTIONS = {
    "dct": "_Z6dct_IVPliPi",
    "fft": "_Z3fftiPlPi",
    "fft_rad2": "_Z7dit_fftPliPK8FIXP_SPKi",
    "mdct": "_Z10mdct_blockP6mdct_tPKsiPliiPK8FIXP_SPKiPs",
    "scale": "_Z11scaleValuesPlii",
    "fixpoint_math": "_Z8fDivNormllPi",
    "quantize": "_Z26FDKaacEnc_QuantizeSpectrumiiiPKiPKliS0_Psi",
    "bit_cnt": "_Z20FDKaacEnc_codeValuesPsiiP13FDK_BITSTREAM",
    "dyn_bits": "_Z21FDKaacEnc_dynBitCountP13BITCNTR_STATEPKsPKjPKiiiiiS6_P12SECTION_DATAS6_S6_S6_j",
    "aacenc_tns": "_Z19FDKaacEnc_TnsDetectP8TNS_DATAPK10TNS_CONFIGP8TNS_INFOiPKlii",
}
CODEC_RAM_TABLES = {"aacEnc_rom": "FDKaacEnc_mTab_3_4"}


def codec_ram_functions(symbols):
    """Check a live function from each selected archive member after linking."""
    result = {}
    for member, name in CODEC_RAM_FUNCTIONS.items():
        matches = re.findall(r"^([0-9a-fA-F]+)\s+[Tt]\s+" + re.escape(name) + r"$",
                             symbols, re.MULTILINE)
        if len(matches) != 1 or not RAM_START <= int(matches[0], 16) < RAM_END:
            raise ValueError(f"Shared codec kernel is not linked into main RAM: {member}")
        result[member] = int(matches[0], 16)
    return result


def codec_ram_tables(symbols):
    """The monolithic encoder ROM member must be copied into RAM with .data."""
    rows = [[field.strip() for field in line.split("|")] for line in symbols.splitlines()]
    result = {}
    for member, name in CODEC_RAM_TABLES.items():
        matches = [row for row in rows if len(row) == 7 and row[0] == name]
        if len(matches) != 1 or matches[0][3] != "OBJECT" or matches[0][6] != ".data":
            raise ValueError(f"Encoder codec tables are not linked into main RAM: {member}")
        try:
            address, size = int(matches[0][1], 16), int(matches[0][4], 16)
        except ValueError as error:
            raise ValueError(f"Invalid encoder codec table symbol: {member}") from error
        if not size or not RAM_START <= address < address + size <= RAM_END:
            raise ValueError(f"Encoder codec tables are not linked into main RAM: {member}")
        # GNU nm calls all symbols in the mixed executable .data section T.
        # Its System-V output retains the actual ELF OBJECT type and section.
        result[member] = address
    return result


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


def verify_library(firmware, codec_sha256, label="FDK"):
    stamps = set(re.findall(rb"APDG_" + label.encode("ascii") + rb"_SHA256:([0-9a-f]{64})\x00", firmware))
    if stamps != {codec_sha256.encode("ascii")}:
        raise ValueError(f"{label} codec archive does not match the fingerprint linked into this firmware")


def phone_worker_bytes(firmware, symbols):
    """Read sizeof(worker_t) retained by this firmware, never the current source layout."""
    firmware_heap(firmware[:52], symbols)
    addresses = re.findall(r"^([0-9a-fA-F]+)\s+[RrDd]\s+airpods_phone_worker_storage_bytes$",
                           symbols, re.MULTILINE)
    if len(addresses) != 1 or int(addresses[0], 16) & 3:
        raise ValueError("Firmware must retain one aligned phone worker storage size")
    address = int(addresses[0], 16)
    offset = struct.unpack_from("<I", firmware, 28)[0]
    entry_size, count = struct.unpack_from("<HH", firmware, 42)
    if entry_size != 32 or not 0 < count <= 128 or offset < 52 or offset + entry_size * count > len(firmware):
        raise ValueError("Unsupported or truncated firmware program headers")
    values = []
    for entry in range(count):
        kind, file_offset, virtual, _, size, memory_size, _, _ = struct.unpack_from(
            "<IIIIIIII", firmware, offset + entry * entry_size)
        if kind != 1:
            continue
        if size > memory_size or file_offset + size > len(firmware) or virtual + size > 1 << 32:
            raise ValueError("Invalid firmware load segment")
        if virtual <= address and address + 4 <= virtual + size:
            values.append(struct.unpack_from("<I", firmware, file_offset + address - virtual)[0])
    if len(values) != 1 or not 4096 <= values[0] <= 65536 or values[0] & 3:
        raise ValueError("Invalid or unmapped phone worker storage size")
    return values[0]


def invalidate_outputs(firmware):
    # Only this ELF's named flash artifacts; never recurse or follow an output symlink.
    for suffix in (".uf2", ".bin", ".hex", ".uf2.pending"):
        firmware.with_suffix(suffix).unlink(missing_ok=True)


def convert_uf2(picotool, firmware, pending, family, abs_block):
    command = [picotool, "uf2", "convert", "--quiet", str(firmware), str(pending),
               "-t", "uf2", "--family", family]
    if abs_block:
        command.append("--abs-block")
    converted = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if converted.returncode or not pending.is_file() or pending.stat().st_size == 0:
        raise ValueError("UF2 conversion failed:\n" + converted.stdout + converted.stderr)


def input_hashes(firmware, library, lhdc_library):
    directory = pathlib.Path(__file__).resolve().parent
    root = directory.parents[1]
    checks = [directory / name for name in ("run.py", "probe.c", "lhdc_quality.h",
                                           "profile_checks.h", "startup.c", "probe.ld")]
    checks += [root / "3rd-party/lhdcv5/lhdc48_bench_fixture.h", root / "tests/lhdc/music_reference.h"]
    checks += [root / "tools/check_usb_audio.py"]
    return {"firmware_sha256": digest(firmware), "codec_sha256": digest(library),
            "lhdc_sha256": digest(lhdc_library),
            "validation_sha256": hashlib.sha256(b"\0".join(path.read_bytes() for path in checks)).hexdigest()}


def current_output(args, firmware, receipt):
    try:
        saved = json.loads(receipt.read_text())
        if not isinstance(saved,dict) or saved.get("usb_audio_mode") != getattr(args,"usb_audio",None):
            return False
        return isinstance(saved, dict) and saved.get("passed") is True and all(saved.get(key) == value for key, value in
            input_hashes(firmware, args.library.resolve(), args.lhdc_library.resolve()).items()) and \
            saved.get("uf2_sha256") == digest(firmware.with_suffix(".uf2"))
    except (ValueError, OSError):
        return False


def run(args):
    directory = pathlib.Path(__file__).resolve().parent
    root = directory.parents[1]
    output = (args.output or root / "build" / "codec-probe").resolve()
    receipt = output / "memory-check.json"
    firmware = args.firmware.resolve()
    if firmware.suffix != ".elf":
        raise ValueError("Expected the linked firmware .elf path")
    # Only the CMake missing-output recovery rule may reuse an unchanged receipt.
    # Explicit rechecks always invalidate and repeat every check.
    if getattr(args, "if_current", False) and current_output(args, firmware, receipt):
        return
    invalidate_outputs(firmware)
    try:
        output.mkdir(parents=True, exist_ok=True)
        # A failed or interrupted recheck cannot leave a stale passing receipt.
        receipt.unlink(missing_ok=True)
        validate(args, directory, root, output, receipt, firmware)
    except BaseException:
        try:
            receipt.unlink(missing_ok=True)
        finally:
            invalidate_outputs(firmware)
        raise


def validate(args, directory, root, output, receipt, firmware):
    library, lhdc_library = args.library.resolve(), args.lhdc_library.resolve()
    if not firmware.is_file() or not library.is_file() or not lhdc_library.is_file():
        raise ValueError("The built firmware ELF, FDK archive and LHDC archive are required")
    compiler = shutil.which(args.compiler)
    nm = shutil.which(args.nm)
    emulator = shutil.which("qemu-system-arm")
    picotool = shutil.which(args.picotool)
    if compiler is None or nm is None or emulator is None or picotool is None:
        raise ValueError("The ARM compiler, matching nm, qemu-system-arm and picotool are required")
    before = input_hashes(firmware, library, lhdc_library)
    usb_formats=None
    usb_mode=getattr(args,"usb_audio",None)
    if usb_mode:
        checked=subprocess.run([sys.executable,str(root/"tools/check_usb_audio.py"),str(firmware),
                                *(["--s16-only"] if usb_mode=="s16" else [])],
                               check=True,capture_output=True,text=True,timeout=10)
        usb_formats=json.loads(checked.stdout)
    firmware_data = firmware.read_bytes()
    verify_library(firmware_data, before["codec_sha256"])
    verify_library(firmware_data, before["lhdc_sha256"], "LHDC")
    symbols = subprocess.run([nm, "--defined-only", str(firmware)], capture_output=True,
                             text=True, timeout=10, check=True).stdout
    capacity = firmware_heap(firmware_data[:52], symbols)
    codec_ram = codec_ram_functions(symbols)
    table_symbols = subprocess.run([nm, "--defined-only", "--format=sysv", str(firmware)],
                                   capture_output=True, text=True, timeout=10, check=True).stdout
    codec_tables = codec_ram_tables(table_symbols)
    worker_bytes = phone_worker_bytes(firmware_data, symbols)
    budget = capacity - RUNTIME_RESERVE_BYTES
    fdk = root / "3rd-party" / "fdk-aac"
    command = [compiler, "-mcpu=cortex-m33", "-mthumb", "-march=armv8-m.main+fp+dsp",
               "-mfloat-abi=softfp", "-mcmse", "-O2", "-g", "-ffreestanding",
               "-Wall", "-Wextra", "-Werror", "-DFDK_AIRPODS_PROFILE=1",
               f"-DPROBE_HEAP_BYTES={budget}", f"-DPROBE_PHONE_WORKER_BYTES={worker_bytes}",
               "-I", str(root / "3rd-party" / "lhdcv5")]
    for module in ("libAACenc", "libAACdec", "libSYS"):
        command += ["-isystem", str(fdk / module / "include")]
    command += ["-nostartfiles", f"-Wl,-T,{directory / 'probe.ld'}",
                str(directory / "startup.c"), str(directory / "probe.c"), str(library), str(lhdc_library),
                "-Wl,--start-group", "-lc", "-lm", "-lnosys", "-Wl,--end-group"]
    modes, builds, transcripts = {}, [], []
    for mode in ("usb_mic", "phone_lhdc"):
        elf = output / f"codec-probe-{mode}.elf"
        compiled = subprocess.run(command + [f"-DPROBE_PHONE_MODE={int(mode == 'phone_lhdc')}",
                                             "-o", str(elf)],
                                  capture_output=True, text=True, timeout=60)
        builds.append(f"MODE {mode}\n" + compiled.stdout + compiled.stderr)
        (output / "build.log").write_text("".join(builds))
        if compiled.returncode:
            raise ValueError("Codec probe compilation failed:\n" + builds[-1])
        result = subprocess.run([emulator, "-M", "mps2-an500", "-nographic",
                                 "-semihosting-config", "enable=on,target=native",
                                 "-kernel", str(elf)],
                                capture_output=True, text=True, timeout=60)
        transcript = result.stdout + result.stderr
        transcripts.append(f"MODE {mode}\n" + transcript)
        (output / "result.log").write_text("".join(transcripts))
        if result.returncode:
            raise ValueError("Codec memory check failed:\n" + transcript)
        if mode == "usb_mic":
            complete = "SUCCESS frames=16 checksum=4105af5d" in transcript
        else:
            complete = re.search(rf"^PHONE_LHDC frames=64 encodes=32 worker_bytes={worker_bytes} checksum=[0-9a-f]{{8}}$",
                                 transcript, re.MULTILINE) is not None
            quality = re.findall(r"^PHONE_QUALITY cycle=([01]) reference_samples=819 "
                                 r"gain_ppm=(\d+)/(\d+) snr_above_30db=1$", transcript, re.MULTILINE)
            complete = complete and [entry[0] for entry in quality] == ["0", "1"] and all(
                995000 <= int(gain) <= 1005000 for entry in quality for gain in entry[1:])
        if not complete or "CHECKS complete" not in transcript:
            raise ValueError("Codec probe did not complete every required check")
        peaks = re.findall(r"^HEAP_PEAK bytes=(\d+)$", transcript, re.MULTILINE)
        if len(peaks) != 1 or not 0 < int(peaks[0]) <= budget:
            raise ValueError("Codec probe returned an invalid heap high-water mark")
        modes[mode] = {"peak_codec_heap_break_bytes": int(peaks[0]),
                       "remaining_after_peak_bytes": capacity - int(peaks[0])}
        if mode == "phone_lhdc":
            modes[mode]["quality"] = [{"reference_pairs": 819, "gain_ppm": [int(entry[1]), int(entry[2])],
                                        "snr_above_30db": True} for entry in quality]
    peak = max(mode["peak_codec_heap_break_bytes"] for mode in modes.values())
    if before != input_hashes(firmware, library, lhdc_library):
        raise ValueError("Firmware or codec archive changed during validation")
    # Convert to a non-flashable sibling first. A failed/interrupted conversion cannot
    # expose a partial UF2 under the documented flash filename.
    pending = firmware.with_suffix(".uf2.pending")
    convert_uf2(picotool, firmware, pending, args.uf2_family, args.uf2_abs_block)
    if before != input_hashes(firmware, library, lhdc_library):
        raise ValueError("Firmware or codec archive changed during UF2 conversion")
    summary = {**before, "uf2_sha256": digest(pending), "heap_capacity_bytes": capacity,
               "codec_ram_function_addresses": codec_ram,
               "codec_ram_table_addresses": codec_tables,
               "usb_audio_mode":usb_mode,"usb_audio_formats":usb_formats,
               "runtime_reserve_bytes": RUNTIME_RESERVE_BYTES, "probe_budget_bytes": budget,
               "phone_worker_storage_bytes": worker_bytes, "modes": modes,
               "peak_codec_heap_break_bytes": peak,
               "remaining_after_peak_bytes": capacity - peak,
               "passed": True, "hardware_tested": False}
    receipt.write_text(json.dumps(summary, indent=2) + "\n")
    pending.replace(firmware.with_suffix(".uf2"))
    print("".join(transcripts), end="")
    print(f"Codec memory check passed: {capacity - peak} bytes remain at the emulated peak; "
          f"{RUNTIME_RESERVE_BYTES} bytes were reserved for other runtime use.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", required=True, type=pathlib.Path,
                        help="exact linked airpods_dongle.elf")
    parser.add_argument("--library", required=True, type=pathlib.Path,
                        help="codec archive linked into that firmware")
    parser.add_argument("--lhdc-library", required=True, type=pathlib.Path,
                        help="exact LHDC archive linked into that firmware")
    parser.add_argument("--compiler", default="arm-none-eabi-gcc")
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    parser.add_argument("--picotool", default="picotool", help="matching Pico SDK picotool executable")
    parser.add_argument("--uf2-family", default="rp2350-arm-s")
    parser.add_argument("--uf2-abs-block", action="store_true", help="Pico SDK RP2350 A2 compatibility flag")
    parser.add_argument("--if-current", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--usb-audio", choices=("s16","float"), help="check linked USB descriptors before publication")
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
