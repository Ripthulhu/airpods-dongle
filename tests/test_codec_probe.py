import argparse
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
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


SYMBOLS = "20020000 B end\n2007c000 A __StackLimit\n"


class CodecMemoryCheckTests(unittest.TestCase):
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

    def test_stamp_changes_only_when_archive_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            archive, generated = Path(directory) / "codec.a", Path(directory) / "stamp.c"
            archive.write_bytes(b"codec archive")
            stamp.stamp(archive, generated)
            expected = hashlib.sha256(archive.read_bytes()).hexdigest()
            self.assertIn(f"APDG_FDK_SHA256:{expected}", generated.read_text())
            timestamp = generated.stat().st_mtime_ns
            stamp.stamp(archive, generated)
            self.assertEqual(generated.stat().st_mtime_ns, timestamp)
            archive.write_bytes(b"changed codec archive")
            stamp.stamp(archive, generated)
            self.assertNotIn(expected, generated.read_text())

    def inputs(self, directory):
        output = Path(directory) / "output"
        output.mkdir()
        (output / "memory-check.json").write_text('{"passed": true}')
        library = Path(directory) / "codec.a"
        library.write_bytes(b"codec archive")
        firmware = Path(directory) / "firmware.elf"
        firmware.write_bytes(elf_header() + b"APDG_FDK_SHA256:" +
                             probe.digest(library).encode() + b"\0")
        return argparse.Namespace(output=output, firmware=firmware, library=library,
                                  compiler="arm-none-eabi-gcc", nm="arm-none-eabi-nm")

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
            calls = 0

            def invoke(command, **kwargs):
                nonlocal calls
                calls += 1
                if calls == 1:
                    return subprocess.CompletedProcess(command, 0, SYMBOLS, "")
                if calls == 2:
                    return subprocess.CompletedProcess(command, 0, "", "")
                args.firmware.write_bytes(args.firmware.read_bytes() + b"changed")
                return subprocess.CompletedProcess(command, 0,
                    "SUCCESS frames=16 checksum=4105af5d\nHEAP_PEAK bytes=100000\nCHECKS complete\n", "")

            with patch.object(probe.shutil, "which", return_value="tool"), \
                 patch.object(probe.subprocess, "run", side_effect=invoke):
                with self.assertRaisesRegex(ValueError, "changed during validation"):
                    probe.run(args)
            self.assertFalse((args.output / "memory-check.json").exists())

    def fake_tools(self, transcript, compile_error=False, qemu_error=False):
        calls = 0

        def invoke(command, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 1:
                return subprocess.CompletedProcess(command, 0, SYMBOLS, "")
            if calls == 2:
                return subprocess.CompletedProcess(command, int(compile_error), "", "compile error" if compile_error else "")
            return subprocess.CompletedProcess(command, int(qemu_error), transcript, "")

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
            transcript = "SUCCESS frames=16 checksum=4105af5d\nHEAP_PEAK bytes=100000\nCHECKS complete\n"
            with patch.object(probe.shutil, "which", return_value="tool"), \
                 patch.object(probe.subprocess, "run", side_effect=self.fake_tools(transcript)), \
                 redirect_stdout(io.StringIO()):
                probe.run(args)
            receipt = json.loads((args.output / "memory-check.json").read_text())
            self.assertTrue(receipt["passed"])
            self.assertFalse(receipt["hardware_tested"])
            self.assertEqual(receipt["firmware_sha256"], probe.digest(args.firmware))
            self.assertEqual(receipt["codec_sha256"], probe.digest(args.library))
            self.assertEqual(receipt["heap_capacity_bytes"], 0x5c000)
            self.assertEqual(receipt["runtime_reserve_bytes"], 65536)
            self.assertEqual(receipt["probe_budget_bytes"], 0x5c000 - 65536)
            self.assertEqual(receipt["peak_codec_heap_break_bytes"], 100000)
            self.assertEqual(receipt["remaining_after_peak_bytes"], 0x5c000 - 100000)


if __name__ == "__main__":
    unittest.main()
