import hashlib
from contextlib import contextmanager
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import make_linker

# The build validates complete pinned SDK includes; these fixtures isolate the
# boundaries while preserving markers for untouched SDK sections and aliases.
SOURCE = {
    "section_heap.incl": b"""/* Original SDK notice. */
    .heap (NOLOAD): {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM
    __HeapLimit = DEFINED(HEAP_LIMIT) ? HEAP_LIMIT : ORIGIN(RAM) + LENGTH(RAM);
""",
    "sections_stack.incl": b"""    /* Untouched SDK core-1 placement. */
    .stack1_dummy (NOLOAD): { *(.stack1*) } > SCRATCH_X
    .stack_dummy (NOLOAD):
    {
        KEEP(*(.stack*))
    } > SCRATCH_Y
""",
    "section_end.incl": b"""    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);
    __StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    __StackBottom = __StackTop - SIZEOF(.stack_dummy);
    PROVIDE (__heap_end = __HeapLimit);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);
    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")
""",
}
LAYOUT_SOURCE = {name: f"Original layout: {name}\n".encode()
                 for name in make_linker.LAYOUT_SHA256}

class LinkerTests(unittest.TestCase):
    @contextmanager
    def fixture_hash(self, sources=SOURCE):
        with patch.object(make_linker, "ORIGINAL_SHA256", {
            name: hashlib.sha256(value).hexdigest() for name, value in sources.items()
        }), patch.object(make_linker, "LAYOUT_SHA256", {
            name: hashlib.sha256(value).hexdigest() for name, value in LAYOUT_SOURCE.items()
        }):
            yield

    def write_inputs(self, sdk, sources=SOURCE):
        directory = sdk / make_linker.SDK_INCLUDE
        directory.mkdir(parents=True)
        for name, value in sources.items():
            (directory / name).write_bytes(value)
        for name, value in LAYOUT_SOURCE.items():
            target = sdk / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(value)

    def test_stack_and_heap_cannot_overlap(self):
        with self.fixture_hash():
            outputs = make_linker.generate(SOURCE)
        generated = "\n".join(value.decode() for value in outputs.values())
        self.assertIn("__HeapLimit = ORIGIN(RAM) + LENGTH(RAM) - 16384;", generated)
        self.assertIn(". = ALIGN(8);\n        __end__ = .;\n        end = __end__;", generated)
        self.assertIn(".stack_dummy __HeapLimit (NOLOAD)", generated)
        self.assertIn("KEEP(*(.stack*))\n    } > RAM", generated)
        self.assertIn("__StackLimit = __HeapLimit;", generated)
        self.assertIn("__StackTop = ORIGIN(RAM) + LENGTH(RAM);", generated)
        self.assertIn("ASSERT(__heap_min_end <= __HeapLimit", generated)
        self.assertIn("ASSERT(SIZEOF(.stack_dummy) == 16384", generated)
        self.assertIn("ASSERT(__StackBottom == __HeapLimit", generated)
        self.assertIn("/* Original SDK notice. */", generated)
        self.assertIn(".stack1_dummy (NOLOAD): { *(.stack1*) } > SCRATCH_X", generated)
        self.assertIn("PROVIDE (__heap_end = __HeapLimit);", generated)
        self.assertIn("PROVIDE (__llvm_libc_heap_limit = __HeapLimit);", generated)
        self.assertEqual(0x20000000 + 512 * 1024 - make_linker.STACK_BYTES, 0x2007C000)

    def test_unknown_input_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unsupported SDK linker"):
            make_linker.generate(SOURCE)
        with self.fixture_hash(), self.assertRaisesRegex(ValueError, "Unsupported SDK linker"):
            make_linker.generate({})

    def test_missing_or_duplicate_context_rejected(self):
        for name, value in [("section_heap.incl", SOURCE["section_heap.incl"].replace(b"KEEP(*(.heap*))", b"changed heap")),
                            ("section_end.incl", SOURCE["section_end.incl"] + b"    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);\n")]:
            sources = dict(SOURCE, **{name: value})
            with self.fixture_hash(sources), self.assertRaisesRegex(ValueError, "Unexpected linker context"):
                make_linker.generate(sources)

    def test_generates_outside_sdk_without_modifying_input(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sdk = root / "sdk"
            self.write_inputs(sdk)
            output = root / "build" / "airpods-linker"
            with self.fixture_hash():
                self.assertEqual(make_linker.main(["--sdk", str(sdk), "--output", str(output)]), 0)
            for name, data in SOURCE.items():
                self.assertEqual((sdk / make_linker.SDK_INCLUDE / name).read_bytes(), data)
            self.assertIn(b"core-0 stack must be 16 KiB", (output / "section_end.incl").read_bytes())

    def test_bad_input_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sdk, output = root / "sdk", root / "airpods-linker"
            self.write_inputs(sdk)
            output.mkdir()
            for name in SOURCE:
                (output / name).write_bytes(b"previous layout")
            with self.assertRaises(ValueError):
                make_linker.generate(SOURCE)
            self.assertEqual(make_linker.main(["--sdk", str(sdk), "--output", str(output)]), 1)
            for name in SOURCE:
                self.assertEqual((output / name).read_bytes(), b"previous layout")

    def test_will_not_write_inside_sdk(self):
        with tempfile.TemporaryDirectory() as temp:
            sdk = Path(temp)
            self.write_inputs(sdk)
            with self.fixture_hash():
                self.assertEqual(make_linker.main(["--sdk", str(sdk), "--output", str(sdk / "override")]), 1)
            for name, data in SOURCE.items():
                self.assertEqual((sdk / make_linker.SDK_INCLUDE / name).read_bytes(), data)

    def test_changed_base_layout_preserves_all_existing_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sdk, output = root / "sdk", root / "airpods-linker"
            self.write_inputs(sdk)
            output.mkdir()
            for name in SOURCE:
                (output / name).write_bytes(b"previous layout")
            for name, data in LAYOUT_SOURCE.items():
                with self.subTest(name=name), self.fixture_hash():
                    (sdk / name).write_bytes(data + b"changed")
                    self.assertEqual(make_linker.main([
                        "--sdk", str(sdk), "--output", str(output)
                    ]), 1)
                    for output_name in SOURCE:
                        self.assertEqual((output / output_name).read_bytes(), b"previous layout")
                    (sdk / name).write_bytes(data)

    def test_child_output_symlink_cannot_modify_external_or_sdk_files(self):
        for destination in ["external", "sdk"]:
            with self.subTest(destination=destination), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                sdk, output = root / "sdk", root / "airpods-linker"
                self.write_inputs(sdk)
                output.mkdir()
                for name in SOURCE:
                    (output / name).write_bytes(b"previous layout")
                target = (sdk / make_linker.SDK_INCLUDE / "section_end.incl"
                          if destination == "sdk" else root / "do-not-overwrite")
                if destination == "external":
                    target.write_bytes(b"external content")
                original = target.read_bytes()
                link = output / "section_end.incl"
                link.unlink()
                try:
                    link.symlink_to(target)
                except (OSError, NotImplementedError) as error:
                    self.skipTest(f"File symlinks unavailable: {error}")
                with self.fixture_hash():
                    self.assertEqual(make_linker.main([
                        "--sdk", str(sdk), "--output", str(output)
                    ]), 1)
                self.assertEqual(target.read_bytes(), original)
                for name in SOURCE:
                    if name != "section_end.incl":
                        self.assertEqual((output / name).read_bytes(), b"previous layout")

if __name__ == "__main__":
    unittest.main()
