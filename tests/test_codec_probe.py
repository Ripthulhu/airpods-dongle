import argparse
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
import shutil
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools/codec_probe" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


probe, stamp = module("run"), module("stamp")
adaptive_spec = importlib.util.spec_from_file_location("adaptive", ROOT / "tools/test_adaptive_codec.py")
adaptive = importlib.util.module_from_spec(adaptive_spec)
adaptive_spec.loader.exec_module(adaptive)


def elf_header():
    result = bytearray(52)
    result[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", result, 16, 2, 40, 1)
    return result


SYMBOLS = ("20020000 B end\n2007c000 A __StackLimit\n"
           "10001000 R airpods_phone_worker_storage_bytes\n"
           "10001004 R airpods_eld_cbr_bandwidth_hz\n") + "".join(
    f"{0x20001000 + index * 0x100:08x} T {name}\n"
    for index, name in enumerate(probe.CODEC_RAM_FUNCTIONS.values()))
TABLE_SYMBOLS = "".join(
    f"{name}|{0x20003000 + index * 0x100:08x}| T |OBJECT|00000400||.data\n"
    for index, name in enumerate(probe.CODEC_RAM_TABLES.values()))
PCM_ABI = "PCM_ABI encoder_bits=32 decoder_bits=32\n"
USB_RESULT = (PCM_ABI + "SUCCESS frames=16 checksum=4105af5d\n"
              "HISTORICAL_FIXTURE mono_bandwidth=auto stereo_bandwidth=0\n"
              "PCM_DECODER complete_q31_output=1 lower16_preserved=1\n"
              "MATRIX_HISTORICAL bandwidth=0 frames=960 configurations=10 fixed_hashes=1\n"
              "CANDIDATE_CBR bandwidth=20000 rates=5 frames=480 retained_mic=1\n"
              "LEGACY_LC_MIC rates=192000,256000 frame=1024 input_bits=16 frames=192 max_au=1024\n"
              "PROFILE_INPUT preinit_info=1 control_state_guard=1 invalid_init=1 valid_retry=1\n"
              "HEAP_PEAK bytes=188040\nCHECKS complete\n")
PHONE_RESULT = (PCM_ABI + "PHONE_QUALITY cycle=0 reference_samples=819 gain_ppm=999345/1001227 snr_above_30db=1\n"
                "PHONE_QUALITY cycle=1 reference_samples=819 gain_ppm=999345/1001227 snr_above_30db=1\n"
                "PHONE_LHDC frames=64 encodes=32 worker_bytes=9772 checksum=1234abcd\n"
                "HEAP_PEAK bytes=120000\nCHECKS complete\n")


def firmware_image(worker_bytes=9772, bandwidth=20000):
    result = elf_header()
    struct.pack_into("<I", result, 28, 52)
    struct.pack_into("<HH", result, 42, 32, 1)
    result += struct.pack("<IIIIIIII", 1, 84, 0x10001000, 0x10001000, 8, 8, 4, 4)
    return result + struct.pack("<II", worker_bytes, bandwidth)


class CodecMemoryCheckTests(unittest.TestCase):
    def test_historical_combined_checksum_uses_auto_bandwidth_without_narrowing_candidate_probes(self):
        source = (ROOT / "tools/codec_probe/probe.c").read_text()
        main = source[source.index('int main(void)'):]
        self.assertIn('HANDLE_AACENCODER mono = encoder(1, 64000);', main)
        self.assertIn('HANDLE_AACENCODER stereo = encoder_bandwidth(2, 256000, 0);', main)
        self.assertNotIn('HANDLE_AACENCODER stereo = encoder(2, 256000);', main)
        self.assertIn('if (channels == 2)\n        ENC_OK(aacEncoder_SetParam(handle, AACENC_BANDWIDTH, bandwidth));',
                      adaptive.function(source, 'encoder_bandwidth'))
        self.assertIn('aaceld_bandwidth(bitrate, 0)', adaptive.function(source, 'encoder'))
        self.assertIn('HANDLE_AACENCODER handle = encoder(2, rates[mode]);',
                      adaptive.function(source, 'check_candidate_cbr'))
        self.assertIn('phone.encoder = encoder(2, 256000);', adaptive.function(source, 'check_phone_lhdc'))
        self.assertIn('playback_encoder(modes[mode].aot, modes[mode].bitrate)',
                      adaptive.function(source, 'check_lifecycle'))

    def run_cpp(self, source, includes=(), sources=(), definitions=()):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "check.cpp"
            executable = Path(directory) / ("check.exe" if shutil.which("cl") else "check")
            path.write_text(source)
            if shutil.which("c++"):
                command = [shutil.which("c++"), "-std=c++11", "-O2"]
                command += ["-D" + define for define in definitions]
                for include in includes:
                    command += ["-I", str(include)]
                command += [str(path), *map(str, sources), "-o", str(executable)]
            else:
                command = [shutil.which("cl"), "/nologo", "/EHsc", "/std:c++17", "/O2"]
                command += ["/D" + define for define in definitions]
                command += ["/I" + str(include) for include in includes]
                command += [str(path), *map(str, sources), "/Fe:" + str(executable)]
            compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(executable)], check=True, capture_output=True, timeout=10)

    @unittest.skipUnless(shutil.which("c++") or shutil.which("cl"), "native C++ compiler required")
    def test_real_sbrk_records_denials_without_allocation_or_logging_and_preserves_budget(self):
        text = (ROOT / "tools/codec_probe/probe.c").read_text()
        start = text.index("void *_sbrk(")
        body = text[start:text.index("static void report_heap", start)]
        source = r'''
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
static char heap_memory[256];
#define _end heap_memory[0]
#define PROBE_HEAP_BYTES 128
static char *heap_break;
static size_t peak_heap_break;
static struct { ptrdiff_t request; size_t current; unsigned count; } heap_denial;
#define malloc allocation_forbidden_inside_sbrk
#define calloc allocation_forbidden_inside_sbrk
#define realloc allocation_forbidden_inside_sbrk
#define printf logging_forbidden_inside_sbrk
#define snprintf logging_forbidden_inside_sbrk
#define probe_emit logging_forbidden_inside_sbrk
''' + body + r'''
int main() {
    assert(_sbrk(64) == heap_memory);
    assert(_sbrk(64) == heap_memory + 64);
    assert(_sbrk(1) == (void *)-1 && errno == ENOMEM);
    assert(heap_denial.count == 1 && heap_denial.request == 1 && heap_denial.current == 128);
    assert(heap_break == heap_memory + 128 && peak_heap_break == 128);
    assert(_sbrk(-129) == (void *)-1 && heap_denial.count == 2 && heap_denial.request == -129);
    assert(_sbrk(-64) == heap_memory + 128 && heap_break == heap_memory + 64);
    assert(_sbrk(32) == heap_memory + 64 && heap_break == heap_memory + 96);
    assert(peak_heap_break == 128 && heap_denial.current == 128 && heap_denial.count == 2);
}
'''
        self.run_cpp(source)

    @unittest.skipUnless(shutil.which("c++") or shutil.which("cl"), "native C++ compiler required")
    def test_workspace_expectations_match_real_aligned_allocator_getters_not_just_payload(self):
        text = (ROOT / "tools/codec_probe/profile_checks.h").read_text()
        helper = adaptive.function(text, "profile_workspace_bytes")
        source = r'''
#include "genericStds.h"
#include <cassert>
#include <cstdint>
void *FDKaalloc_L(UINT, UINT, MEMORY_SECTION) { assert(false); return nullptr; }
void FDKafree_L(void *) { assert(false); }
C_AALLOC_MEM_L(ProbeSpectral, int32_t, 1024, SECT_DATA_L2)
C_AALLOC_MEM_L(ProbeTime, int32_t, 960, SECT_DATA_EXTERN)
''' + helper + r'''
int main() {
    assert(GetRequiredMemProbeSpectral() == profile_workspace_bytes(4096));
    assert(GetRequiredMemProbeTime() == profile_workspace_bytes(3840));
    assert(GetRequiredMemProbeSpectral() > 4096 && GetRequiredMemProbeTime() > 3840);
    for (unsigned pointer_bytes = 4; pointer_bytes <= 8; pointer_bytes += 4) {
        assert(ALGN_SIZE_EXTRES(4096 + ALIGNMENT_DEFAULT + pointer_bytes) == 4112);
        assert(ALGN_SIZE_EXTRES(3840 + ALIGNMENT_DEFAULT + pointer_bytes) == 3856);
    }
}
'''
        self.run_cpp(source, includes=[ROOT / "3rd-party/fdk-aac/libSYS/include"])

    def test_frontend_wrapper_requires_exactly_one_real_wide_copy_overload(self):
        for kind in ("i", "l"):
            name = f"_Z16FDK_deinterleavePK{kind}P{kind}jjj"
            self.assertEqual(adaptive.frontend_symbol("00000000 T " + name + "\n"), name)
        for symbols in ("", "00000000 T _Z16FDK_deinterleavePKlPsjjj\n",
                        "00000000 T _Z16FDK_deinterleavePKlPljjj\n" * 2,
                        "00000000 T _Z16FDK_deinterleavePKlPljjj\n00000000 T _Z16FDK_deinterleavePKiPijjj\n"):
            self.assertRaises(ValueError, adaptive.frontend_symbol, symbols)

    @unittest.skipUnless(shutil.which("c++") or shutil.which("cl"), "native C++ compiler required")
    def test_real_frontend_preserves_low16_with_legacy_truncation_negative_control(self):
        source = r'''
#include "pcm_utils.h"
#include <cassert>
#include <cstdint>
#include <cstring>
static_assert(sizeof(INT_PCM) == 4 && SAMPLE_BITS == 32, "Q31 ABI required");
int main() {
    const INT_PCM first[] = {65537, -65535, 98305, -32767, 131073, -131071, 163841, -98303};
    const INT_PCM second[] = {65538, -65534, 98306, -32766, 131074, -131070, 163842, -98302};
    INT_PCM wide[2][12]; SHORT narrow[2][12];
    for (unsigned variant = 0; variant < 2; ++variant) {
        const INT_PCM *input = variant ? second : first;
        for (unsigned n = 0; n < 12; ++n) {wide[variant][n] = 0x5a5a5a5a; narrow[variant][n] = 0x5a5a;}
        FDK_deinterleave(input, wide[variant], 2, 4, 6);
        FDK_deinterleave(input, narrow[variant], 2, 4, 6);
        unsigned lost = 0;
        for (unsigned channel = 0; channel < 2; ++channel) {
            for (unsigned n = 0; n < 4; ++n) {
                unsigned at = channel * 6 + n;
                assert(wide[variant][at] == input[n * 2 + channel]);
                lost += int64_t(narrow[variant][at]) * 65536 != input[n * 2 + channel];
            }
            assert(wide[variant][channel * 6 + 4] == 0x5a5a5a5a);
            assert(wide[variant][channel * 6 + 5] == 0x5a5a5a5a);
        }
        assert(lost == 8);
    }
    assert(std::memcmp(wide[0], wide[1], sizeof(wide[0])) != 0);
    assert(std::memcmp(narrow[0], narrow[1], sizeof(narrow[0])) == 0);
    for (int base = -32768; base < 32768; base += 256) {
        SHORT input[256]; INT_PCM converted[260];
        for (unsigned n = 0; n < 256; ++n) input[n] = static_cast<SHORT>(base + static_cast<int>(n));
        for (unsigned n = 0; n < 260; ++n) converted[n] = 0x5a5a5a5a;
        FDK_deinterleave(input, converted, 2, 128, 130);
        for (unsigned channel = 0; channel < 2; ++channel) {
            for (unsigned n = 0; n < 128; ++n)
                assert(converted[channel * 130 + n] == int64_t(input[n * 2 + channel]) * 65536);
            assert(converted[channel * 130 + 128] == 0x5a5a5a5a);
            assert(converted[channel * 130 + 129] == 0x5a5a5a5a);
        }
    }
    return 0;
}
'''
        fdk = ROOT / "3rd-party/fdk-aac"
        self.run_cpp(source, includes=[fdk / part / "include" for part in ("libSYS", "libFDK", "libPCMutils")],
                     sources=[fdk / "libPCMutils/src/pcm_utils.cpp"], definitions=["FDK_DONGLE_PCM32=1"])

    def test_encoder_rom_must_be_linked_into_main_ram(self):
        self.assertEqual(set(probe.codec_ram_tables(TABLE_SYMBOLS)), set(probe.CODEC_RAM_TABLES))
        for index, (member, name) in enumerate(probe.CODEC_RAM_TABLES.items()):
            line = f"{name}|{0x20003000 + index * 0x100:08x}| T |OBJECT|00000400||.data\n"
            for invalid in ("", line.replace("20003000", "10001000"),
                            line.replace("20003000", "20080000"),
                            line.replace("20003000", "2007fffe"),
                            line.replace("20003000", "1ffffffe"),
                            line.replace("20003000", "invalid"),
                            line.replace("00000400", "00000000"),
                            line.replace("OBJECT", "FUNC"), line.replace(".data", ".bss"),
                            line + line):
                with self.subTest(member=member, invalid=invalid):
                    self.assertRaises(ValueError, probe.codec_ram_tables, TABLE_SYMBOLS.replace(line, invalid))

    def test_each_selected_codec_member_must_be_linked_into_main_ram(self):
        checked = probe.codec_ram_functions(SYMBOLS)
        self.assertEqual(set(checked), set(probe.CODEC_RAM_FUNCTIONS))
        for index, (member, name) in enumerate(probe.CODEC_RAM_FUNCTIONS.items()):
            line = f"{0x20001000 + index * 0x100:08x} T {name}\n"
            for invalid in ("", f"10001000 T {name}\n", f"20080000 T {name}\n",
                            f"1ffffffe T {name}\n", f"20001000 B {name}\n", line + line):
                with self.subTest(member=member, invalid=invalid):
                    self.assertRaises(ValueError, probe.codec_ram_functions, SYMBOLS.replace(line, invalid))

    def test_budget_comes_from_firmware_symbols(self):
        self.assertEqual(probe.firmware_heap(elf_header(), SYMBOLS), 0x5c000)

    def test_unknown_or_truncated_elf_is_rejected(self):
        self.assertRaises(ValueError, probe.firmware_heap, bytes(51), SYMBOLS)
        for offset, value in [(0, 0), (4, 2), (5, 2), (6, 2), (16, 1), (18, 62), (20, 0)]:
            header = elf_header()
            header[offset] = value
            self.assertRaises(ValueError, probe.firmware_heap, header, SYMBOLS)

    def test_missing_duplicate_or_impossible_heap_is_rejected(self):
        for symbols in ["", "20020000 B end\n", SYMBOLS + "20030000 B end\n",
                        "20000000 B end\n20090000 A __StackLimit\n",
                        "20020001 B end\n2007c000 A __StackLimit\n",
                        "10020000 B end\n2007c000 A __StackLimit\n",
                        "2007c000 B end\n20020000 A __StackLimit\n",
                        "20070000 B end\n2007c000 A __StackLimit\n"]:
            self.assertRaises(ValueError, probe.firmware_heap, elf_header(), symbols)

    def test_archive_fingerprint_is_required_and_exact(self):
        digest = "a" * 64
        matching = b"ELFprefixAPDG_FDK_SHA256:" + digest.encode() + b"\0suffix"
        probe.verify_library(matching, digest)
        for data in [b"", matching[:-7], matching.replace(b"a" * 64, b"b" * 64),
                     matching + b"APDG_FDK_SHA256:" + b"c" * 64 + b"\0"]:
            self.assertRaises(ValueError, probe.verify_library, data, digest)

    def test_lhdc_archive_requires_its_own_exact_fingerprint(self):
        digest = "a" * 64
        matching = b"APDG_LHDC_SHA256:" + digest.encode() + b"\0"
        probe.verify_library(matching, digest, "LHDC")
        for data in [b"", matching[:-1], matching.replace(b"a" * 64, b"b" * 64),
                     matching.replace(b"LHDC", b"FDK"),
                     matching + b"APDG_LHDC_SHA256:" + b"c" * 64 + b"\0"]:
            self.assertRaises(ValueError, probe.verify_library, data, digest, "LHDC")

    def test_phone_worker_layout_comes_from_the_linked_elf(self):
        self.assertEqual(probe.phone_worker_bytes(firmware_image(), SYMBOLS), 9772)
        self.assertEqual(probe.phone_worker_bytes(firmware_image(12000), SYMBOLS), 12000)

    def test_missing_duplicate_or_invalid_phone_layout_is_rejected(self):
        for symbols in [SYMBOLS.replace("10001000 R airpods_phone_worker_storage_bytes\n", ""),
                        SYMBOLS + "10001000 R airpods_phone_worker_storage_bytes\n",
                        SYMBOLS.replace("10001000 R", "10001001 R"),
                        SYMBOLS.replace("10001000 R", "10002000 R")]:
            self.assertRaises(ValueError, probe.phone_worker_bytes, firmware_image(), symbols)
        for size in [0, 4, 4095, 9773, 65540]:
            self.assertRaises(ValueError, probe.phone_worker_bytes, firmware_image(size), SYMBOLS)

    def test_phone_layout_load_mapping_checks_all_file_bounds(self):
        image = firmware_image()
        for length in [0, 51, 52, 83, 87]:
            self.assertRaises(ValueError, probe.phone_worker_bytes, image[:length], SYMBOLS)
        for offset, width, value in [(28, 4, 0), (28, 4, 0xfffffff0),
                                     (42, 2, 31), (44, 2, 0), (44, 2, 129),
                                     (56, 4, 0xfffffff0), (68, 4, 9), (72, 4, 3)]:
            damaged = bytearray(image)
            struct.pack_into("<I" if width == 4 else "<H", damaged, offset, value)
            self.assertRaises(ValueError, probe.phone_worker_bytes, damaged, SYMBOLS)
        overlapping = bytearray(image)
        struct.pack_into("<H", overlapping, 44, 2)
        struct.pack_into("<I", overlapping, 56, 116)
        overlapping = overlapping[:84] + overlapping[52:84] + overlapping[84:]
        self.assertRaises(ValueError, probe.phone_worker_bytes, overlapping, SYMBOLS)

    def test_stamp_changes_only_when_archive_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            archive, generated = Path(directory) / "codec.a", Path(directory) / "stamp.c"
            archive.write_bytes(b"codec archive")
            stamp.stamp(archive, generated)
            expected = hashlib.sha256(archive.read_bytes()).hexdigest()
            self.assertIn(f"APDG_FDK_SHA256:{expected}", generated.read_text())
            self.assertIn("airpods_codec_fingerprint[]", generated.read_text())
            timestamp = generated.stat().st_mtime_ns
            stamp.stamp(archive, generated)
            self.assertEqual(generated.stat().st_mtime_ns, timestamp)
            archive.write_bytes(b"changed codec archive")
            stamp.stamp(archive, generated)
            self.assertNotIn(expected, generated.read_text())

    def test_lhdc_stamp_uses_a_separate_symbol_and_preserves_default(self):
        with tempfile.TemporaryDirectory() as directory:
            archive, generated = Path(directory) / "lhdc.a", Path(directory) / "stamp.c"
            archive.write_bytes(b"LHDC archive")
            stamp.stamp(archive, generated, "LHDC")
            self.assertIn("airpods_lhdc_fingerprint", generated.read_text())
            self.assertIn(f"APDG_LHDC_SHA256:{probe.digest(archive)}", generated.read_text())
            for label in ["", "lower", 'FDK";', "A-B"]:
                self.assertRaises(ValueError, stamp.stamp, archive, generated, label)

    def inputs(self, directory):
        output = Path(directory) / "output"
        output.mkdir()
        (output / "memory-check.json").write_text('{"passed": true}')
        library = Path(directory) / "codec.a"
        library.write_bytes(b"codec archive")
        lhdc_library = Path(directory) / "lhdc.a"
        lhdc_library.write_bytes(b"LHDC archive")
        self.active_lhdc_library = lhdc_library.resolve()
        firmware = Path(directory) / "firmware.elf"
        firmware.write_bytes(firmware_image() + b"APDG_FDK_SHA256:" +
                             probe.digest(library).encode() + b"\0APDG_LHDC_SHA256:" +
                             probe.digest(lhdc_library).encode() + b"\0")
        for suffix in (".uf2", ".bin", ".hex", ".uf2.pending"):
            firmware.with_suffix(suffix).write_bytes(b"stale flash artifact")
        return argparse.Namespace(output=output, firmware=firmware, library=library,
                                  lhdc_library=lhdc_library,
                                  compiler="arm-none-eabi-gcc", nm="arm-none-eabi-nm",
                                  picotool="picotool", uf2_family="rp2350-arm-s", uf2_abs_block=True)

    def test_timeout_invalidates_old_success_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.inputs(directory)
            with patch.object(probe.shutil, "which", return_value="tool"), \
                 patch.object(probe.subprocess, "run", side_effect=subprocess.TimeoutExpired("nm", 10)):
                self.assertRaises(subprocess.TimeoutExpired, probe.run, args)
            self.assertFalse((args.output / "memory-check.json").exists())

    def test_changed_firmware_cannot_pass_using_an_old_run(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.inputs(directory)
            def invoke(command, **kwargs):
                if "--defined-only" in command:
                    return subprocess.CompletedProcess(command, 0, TABLE_SYMBOLS if "--format=sysv" in command else SYMBOLS, "")
                if "-kernel" not in command:
                    return subprocess.CompletedProcess(command, 0, "", "")
                args.firmware.write_bytes(args.firmware.read_bytes() + b"changed")
                return subprocess.CompletedProcess(command, 0,
                    PHONE_RESULT if "phone_lhdc" in command[-1] else USB_RESULT, "")

            with patch.object(probe.shutil, "which", return_value="tool"), \
                 patch.object(probe.subprocess, "run", side_effect=invoke):
                with self.assertRaisesRegex(ValueError, "changed during validation"):
                    probe.run(args)
            self.assertFalse((args.output / "memory-check.json").exists())

    def fake_tools(self, transcript=USB_RESULT, compile_error=False, qemu_error=False,
                   phone_transcript=PHONE_RESULT):
        def invoke(command, **kwargs):
            if "--defined-only" in command:
                return subprocess.CompletedProcess(command, 0, TABLE_SYMBOLS if "--format=sysv" in command else SYMBOLS, "")
            if "convert" in command:
                self.assertEqual(command[command.index("-t") + 1], "uf2")
                Path(command[5]).write_bytes(b"validated UF2")
                return subprocess.CompletedProcess(command, 0, "", "")
            if "-kernel" not in command:
                self.assertIn(str(self.active_lhdc_library), command)
                self.assertIn("-DPROBE_PHONE_WORKER_BYTES=9772", command)
                self.assertIn("-DFDK_DONGLE_PCM32=1", command)
                self.assertIn("-DAACELD_CBR_BANDWIDTH=20000", command)
                return subprocess.CompletedProcess(command, int(compile_error), "", "compile error" if compile_error else "")
            return subprocess.CompletedProcess(command, int(qemu_error),
                phone_transcript if "phone_lhdc" in command[-1] else transcript, "")

        return invoke

    def test_missing_tools_invalidate_old_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.inputs(directory)
            with patch.object(probe.shutil, "which", return_value=None), \
                 patch.object(probe.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "required"):
                    probe.run(args)
                run.assert_not_called()
            self.assertFalse((args.output / "memory-check.json").exists())

    def test_missing_or_changed_lhdc_archive_invalidates_old_receipt(self):
        for missing in [False, True]:
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                if missing:
                    args.lhdc_library.unlink()
                else:
                    args.lhdc_library.write_bytes(b"different LHDC archive")
                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run") as run:
                    self.assertRaises(ValueError, probe.run, args)
                    run.assert_not_called()
                self.assertFalse((args.output / "memory-check.json").exists())

    def test_lhdc_changed_during_validation_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.inputs(directory)
            invoke = self.fake_tools()

            def mutate(command, **kwargs):
                result = invoke(command, **kwargs)
                if "-kernel" in command:
                    args.lhdc_library.write_bytes(b"changed during validation")
                return result

            with patch.object(probe.shutil, "which", return_value="tool"), \
                 patch.object(probe.subprocess, "run", side_effect=mutate), \
                 redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError, "changed during validation"):
                    probe.run(args)
            self.assertFalse((args.output / "memory-check.json").exists())

    def test_compile_and_emulator_failures_cannot_leave_success(self):
        for compile_error, qemu_error in [(True, False), (False, True)]:
            with self.subTest(compile=compile_error, qemu=qemu_error), \
                 tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run", side_effect=self.fake_tools("FAILED", compile_error, qemu_error)):
                    with self.assertRaisesRegex(ValueError, "failed"):
                        probe.run(args)
                self.assertFalse((args.output / "memory-check.json").exists())

    def test_incomplete_or_over_budget_probe_is_rejected(self):
        for transcript in [
            "HEAP_PEAK bytes=100000\nCHECKS complete\n",
            "SUCCESS frames=16 checksum=4105af5d\nHEAP_PEAK bytes=100000\n",
            "SUCCESS frames=16 checksum=4105af5d\nCHECKS complete\n",
            "SUCCESS frames=16 checksum=4105af5d\nHEAP_PEAK bytes=0\nCHECKS complete\n",
            "SUCCESS frames=16 checksum=4105af5d\nHEAP_PEAK bytes=999999\nCHECKS complete\n",
        ]:
            with self.subTest(transcript=transcript), tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run", side_effect=self.fake_tools(transcript)):
                    self.assertRaises(ValueError, probe.run, args)
                self.assertFalse((args.output / "memory-check.json").exists())

    def test_success_receipt_binds_budget_peak_reserve_and_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.inputs(directory)
            with patch.object(probe.shutil, "which", return_value="tool"), \
                 patch.object(probe.subprocess, "run", side_effect=self.fake_tools()), \
                 redirect_stdout(io.StringIO()):
                probe.run(args)
            receipt = json.loads((args.output / "memory-check.json").read_text())
            self.assertTrue(receipt["passed"])
            self.assertFalse(receipt["hardware_tested"])
            self.assertEqual(receipt["firmware_sha256"], probe.digest(args.firmware))
            self.assertEqual(receipt["codec_sha256"], probe.digest(args.library))
            self.assertEqual(receipt["lhdc_sha256"], probe.digest(args.lhdc_library))
            self.assertEqual(receipt["uf2_sha256"], probe.digest(args.firmware.with_suffix(".uf2")))
            self.assertEqual(receipt["heap_capacity_bytes"], 0x5c000)
            self.assertEqual(receipt["runtime_reserve_bytes"], 65536)
            self.assertEqual(receipt["probe_budget_bytes"], 0x5c000 - 65536)
            self.assertEqual(receipt["phone_worker_storage_bytes"], 9772)
            self.assertEqual(receipt["fdk_pcm_bits"], 32)
            self.assertEqual(receipt["eld_cbr_bandwidth_hz"], 20000)
            self.assertEqual(receipt["probe_only_heap_diagnostic_bss_bytes"], 12)
            self.assertEqual(receipt["peak_codec_heap_break_bytes"], 188040)
            self.assertEqual(receipt["remaining_after_peak_bytes"], 0x5c000 - 188040)
            self.assertEqual(receipt["modes"]["usb_mic"]["peak_codec_heap_break_bytes"], 188040)
            self.assertEqual(receipt["modes"]["phone_lhdc"]["peak_codec_heap_break_bytes"], 120000)
            self.assertEqual(len(receipt["modes"]["phone_lhdc"]["quality"]), 2)
            self.assertFalse(args.firmware.with_suffix(".uf2.pending").exists())

    def test_phone_mode_must_complete_with_the_linked_worker_layout_and_budget(self):
        for transcript in [USB_RESULT, PHONE_RESULT.replace("64", "32"),
                           PHONE_RESULT.replace("9772", "10000"),
                           PHONE_RESULT.replace("CHECKS complete\n", ""),
                           PHONE_RESULT.replace("cycle=1", "cycle=0"),
                           PHONE_RESULT.replace("reference_samples=819", "reference_samples=1"),
                           PHONE_RESULT.replace("999345", "900000"),
                           PHONE_RESULT.replace("cycle=1 reference_samples=819 gain_ppm=999345",
                                                "cycle=1 reference_samples=819 gain_ppm=900000"),
                           PHONE_RESULT.replace("snr_above_30db=1", "snr_above_30db=0"),
                           PHONE_RESULT.replace("120000", "999999"),
                           PHONE_RESULT + "HEAP_PEAK bytes=1\n"]:
            with self.subTest(transcript=transcript), tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run", side_effect=self.fake_tools(phone_transcript=transcript)):
                    self.assertRaises(ValueError, probe.run, args)
                self.assertFalse((args.output / "memory-check.json").exists())

    def test_linked_bandwidth_requires_an_exact_supported_firmware_word(self):
        for bandwidth in (0, 16000, 18000, 20000):
            self.assertEqual(probe.eld_cbr_bandwidth(firmware_image(bandwidth=bandwidth), SYMBOLS), bandwidth)
        for bandwidth in (1, 15500, 22000):
            self.assertRaises(ValueError, probe.eld_cbr_bandwidth, firmware_image(bandwidth=bandwidth), SYMBOLS)
        for symbols in (SYMBOLS.replace("10001004 R airpods_eld_cbr_bandwidth_hz\n", ""),
                        SYMBOLS + "10001004 R airpods_eld_cbr_bandwidth_hz\n",
                        SYMBOLS.replace("10001004 R", "10001005 R"),
                        SYMBOLS.replace("10001004 R", "10002000 R")):
            self.assertRaises(ValueError, probe.eld_cbr_bandwidth, firmware_image(), symbols)

    def test_requested_bandwidth_mismatch_cannot_compile_or_publish(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.inputs(directory)
            args.eld_cbr_bandwidth = 18000
            with patch.object(probe.shutil, "which", return_value="tool"), \
                 patch.object(probe.subprocess, "run", side_effect=self.fake_tools()) as tools:
                self.assertRaisesRegex(ValueError, "does not match linked", probe.run, args)
            self.assertTrue(all("-kernel" not in call.args[0] and "-mcpu=cortex-m33" not in call.args[0]
                                for call in tools.call_args_list))
            self.assertFalse(args.firmware.with_suffix(".uf2").exists())

    def test_old_16_bit_or_incomplete_32_bit_results_cannot_pass(self):
        for transcript in (USB_RESULT.replace(PCM_ABI, ""), USB_RESULT.replace("encoder_bits=32", "encoder_bits=16"),
                           USB_RESULT.replace("decoder_bits=32", "decoder_bits=16"), USB_RESULT + PCM_ABI,
                           USB_RESULT.replace("HISTORICAL_FIXTURE mono_bandwidth=auto stereo_bandwidth=0\n", ""),
                           USB_RESULT.replace("PCM_DECODER complete_q31_output=1 lower16_preserved=1\n", ""),
                           USB_RESULT.replace("lower16_preserved=1", "lower16_preserved=0")):
            with self.subTest(transcript=transcript), tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run", side_effect=self.fake_tools(transcript)):
                    self.assertRaises(ValueError, probe.run, args)
                self.assertFalse(args.firmware.with_suffix(".uf2").exists())

    def test_historical_only_or_wrong_bandwidth_cannot_stand_in_for_candidate_memory_coverage(self):
        for transcript in (USB_RESULT.replace("CANDIDATE_CBR bandwidth=20000 rates=5 frames=480 retained_mic=1\n", ""),
                           USB_RESULT.replace("CANDIDATE_CBR bandwidth=20000", "CANDIDATE_CBR bandwidth=0"),
                           USB_RESULT.replace("MATRIX_HISTORICAL bandwidth=0 frames=960 configurations=10 fixed_hashes=1\n", ""),
                           USB_RESULT.replace("frames=480 retained_mic=1", "frames=480 retained_mic=0")):
            with self.subTest(transcript=transcript), tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run", side_effect=self.fake_tools(transcript)):
                    self.assertRaises(ValueError, probe.run, args)
                self.assertFalse(args.firmware.with_suffix(".uf2").exists())

    def test_legacy_lc_must_retain_microphone_and_s16_frontend_with_original_au_bound(self):
        line = "LEGACY_LC_MIC rates=192000,256000 frame=1024 input_bits=16 frames=192 max_au=1024\n"
        for replacement in ("", line.replace("256000", "128000"), line.replace("frame=1024", "frame=480"),
                            line.replace("input_bits=16", "input_bits=32"), line.replace("max_au=1024", "max_au=2048")):
            with self.subTest(replacement=replacement), tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run", side_effect=self.fake_tools(USB_RESULT.replace(line, replacement))):
                    self.assertRaises(ValueError, probe.run, args)
                self.assertFalse(args.firmware.with_suffix(".uf2").exists())

    def test_real_public_initialization_boundaries_cannot_be_omitted(self):
        marker = "PROFILE_INPUT preinit_info=1 control_state_guard=1 invalid_init=1 valid_retry=1\n"
        for replacement in ("", marker.replace("control_state_guard=1", "control_state_guard=0")):
            with tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run", side_effect=self.fake_tools(USB_RESULT.replace(marker, replacement))):
                    self.assertRaises(ValueError, probe.run, args)
                self.assertFalse(args.firmware.with_suffix(".uf2").exists())

    def test_failed_or_interrupted_recheck_removes_only_its_flash_artifacts(self):
        for error in (ValueError("failed"), KeyboardInterrupt(), subprocess.TimeoutExpired("qemu", 60)):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                unrelated = args.firmware.with_name("other.uf2")
                unrelated.write_bytes(b"keep")
                with patch.object(probe, "validate", side_effect=error):
                    self.assertRaises(type(error), probe.run, args)
                self.assertEqual(unrelated.read_bytes(), b"keep")
                self.assertTrue(args.firmware.exists())
                self.assertFalse((args.output / "memory-check.json").exists())
                for suffix in (".uf2", ".bin", ".hex", ".uf2.pending"):
                    self.assertFalse(args.firmware.with_suffix(suffix).exists())

    def test_failed_partial_conversion_or_changed_input_cannot_publish(self):
        for failure in ("exit", "interrupt", "timeout", "changed", "empty"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                invoke = self.fake_tools()

                def convert(command, **kwargs):
                    if "convert" not in command:
                        return invoke(command, **kwargs)
                    pending = Path(command[5])
                    pending.write_bytes(b"" if failure == "empty" else b"partial UF2")
                    self.assertFalse(args.firmware.with_suffix(".uf2").exists())
                    if failure == "interrupt":
                        raise KeyboardInterrupt()
                    if failure == "timeout":
                        raise subprocess.TimeoutExpired(command, 60)
                    if failure == "changed":
                        args.firmware.write_bytes(args.firmware.read_bytes() + b"changed")
                    return subprocess.CompletedProcess(command, int(failure == "exit"), "", "failed")

                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run", side_effect=convert):
                    self.assertRaises((ValueError, KeyboardInterrupt, subprocess.TimeoutExpired), probe.run, args)
                self.assertFalse(args.firmware.with_suffix(".uf2").exists())
                self.assertFalse(args.firmware.with_suffix(".uf2.pending").exists())
                self.assertFalse((args.output / "memory-check.json").exists())

    def test_failed_final_rename_removes_passing_receipt_and_pending_output(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.inputs(directory)
            with patch.object(probe.shutil, "which", return_value="tool"), \
                 patch.object(probe.subprocess, "run", side_effect=self.fake_tools()), \
                 patch.object(Path, "replace", side_effect=OSError("rename failed")):
                self.assertRaises(OSError, probe.run, args)
            self.assertFalse(args.firmware.with_suffix(".uf2").exists())
            self.assertFalse(args.firmware.with_suffix(".uf2.pending").exists())
            self.assertFalse((args.output / "memory-check.json").exists())

    def test_missing_output_recovery_reuses_only_unchanged_validated_inputs(self):
        for changed in (None, "firmware", "library", "lhdc_library", "uf2", "validation", "receipt", "pcm_bits", "bandwidth"):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as directory:
                args = self.inputs(directory)
                with patch.object(probe.shutil, "which", return_value="tool"), \
                     patch.object(probe.subprocess, "run", side_effect=self.fake_tools()), \
                     redirect_stdout(io.StringIO()):
                    probe.run(args)
                args.if_current = True
                if changed in ("firmware", "library", "lhdc_library"):
                    path = getattr(args, changed)
                    path.write_bytes(path.read_bytes() + b"changed")
                elif changed == "uf2":
                    args.firmware.with_suffix(".uf2").unlink()
                elif changed == "validation":
                    receipt = args.output / "memory-check.json"
                    data = json.loads(receipt.read_text())
                    data["validation_sha256"] = "changed"
                    receipt.write_text(json.dumps(data))
                elif changed == "receipt":
                    (args.output / "memory-check.json").write_text("[]")
                elif changed in ("pcm_bits", "bandwidth"):
                    receipt = args.output / "memory-check.json"
                    data = json.loads(receipt.read_text())
                    data["fdk_pcm_bits" if changed == "pcm_bits" else "eld_cbr_bandwidth_hz"] = 16 if changed == "pcm_bits" else 18000
                    receipt.write_text(json.dumps(data))
                with patch.object(probe, "validate", side_effect=ValueError("rerun validation")) as validate:
                    if changed is None:
                        probe.run(args)
                        validate.assert_not_called()
                    else:
                        self.assertRaisesRegex(ValueError, "rerun validation", probe.run, args)
                        validate.assert_called_once()
                        self.assertFalse(args.firmware.with_suffix(".uf2").exists())

    @unittest.skipUnless(shutil.which("cc"), "native C compiler required")
    def test_pcm_quality_rejects_silence_gain_and_channel_regressions(self):
        source = r'''
#include "lhdc_quality.h"
#include <assert.h>
#include <string.h>
int main(void) {
    for (unsigned kind = 0; kind < 5; ++kind) {
        lhdc_quality_t quality = {0};
        for (unsigned frame = 0; frame < 32; ++frame) {
            int16_t pcm[480] = {0};
            for (unsigned n = 0; n < 240; ++n) {
                unsigned sample = frame * 240 + n;
                if (sample < 1128 || (sample - 1128) % 8) continue;
                unsigned index = (sample - 1128) / 8;
                if (index >= LHDC_QUALITY_PAIRS) continue;
                for (unsigned c = 0; c < 2; ++c) {
                    int value = lhdc_music_reference[index * 2 + (kind == 3 ? 1 - c : c)];
                    if (kind == 1) value = 0;
                    if (kind == 2) value = value * 9 / 10;
                    if (kind == 4) value = value * 101 / 100;
                    pcm[n * 2 + c] = value;
                }
            }
            lhdc_quality_collect(&quality, pcm, 240);
        }
        assert(lhdc_quality_passed(&quality) == (kind == 0));
    }
    lhdc_quality_t incomplete = {0};
    assert(!lhdc_quality_passed(&incomplete));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path, executable = Path(directory) / "quality.c", Path(directory) / "quality"
            source_path.write_text(source)
            subprocess.run([shutil.which("cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "tools/codec_probe"), str(source_path), "-o", str(executable)],
                           check=True, capture_output=True, timeout=30)
            subprocess.run([str(executable)], check=True, capture_output=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
