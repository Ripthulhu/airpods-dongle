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


def elf_header():
    result = bytearray(52)
    result[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", result, 16, 2, 40, 1)
    return result


SYMBOLS = ("20020000 B end\n2007c000 A __StackLimit\n"
           "10001000 R airpods_phone_worker_storage_bytes\n") + "".join(
    f"{0x20001000 + index * 0x100:08x} T {name}\n"
    for index, name in enumerate(probe.CODEC_RAM_FUNCTIONS.values()))
TABLE_SYMBOLS = "".join(
    f"{name}|{0x20003000 + index * 0x100:08x}| T |OBJECT|00000400||.data\n"
    for index, name in enumerate(probe.CODEC_RAM_TABLES.values()))
USB_RESULT = "SUCCESS frames=16 checksum=4105af5d\nHEAP_PEAK bytes=188040\nCHECKS complete\n"
PHONE_RESULT = ("PHONE_QUALITY cycle=0 reference_samples=819 gain_ppm=999345/1001227 snr_above_30db=1\n"
                "PHONE_QUALITY cycle=1 reference_samples=819 gain_ppm=999345/1001227 snr_above_30db=1\n"
                "PHONE_LHDC frames=64 encodes=32 worker_bytes=9772 checksum=1234abcd\n"
                "HEAP_PEAK bytes=120000\nCHECKS complete\n")


def firmware_image(worker_bytes=9772):
    result = elf_header()
    struct.pack_into("<I", result, 28, 52)
    struct.pack_into("<HH", result, 42, 32, 1)
    result += struct.pack("<IIIIIIII", 1, 84, 0x10001000, 0x10001000, 4, 4, 4, 4)
    return result + struct.pack("<I", worker_bytes)


class CodecMemoryCheckTests(unittest.TestCase):
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
                                     (56, 4, 0xfffffff0), (68, 4, 5), (72, 4, 3)]:
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
        for changed in (None, "firmware", "library", "lhdc_library", "uf2", "validation", "receipt"):
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
