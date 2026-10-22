import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import make_linker


# The build exercises the complete pinned SDK file; this fixture isolates the
# stack/heap boundaries and leaves a marker for the untouched SDK sections.
SOURCE = b"""/* Original SDK notice. */
MEMORY { RAM(rwx) : ORIGIN = 0x20000000, LENGTH = 512k }
    .heap (NOLOAD): {
        KEEP(*(.heap*))
    } > RAM
    __HeapLimit = ORIGIN(RAM) + LENGTH(RAM);
    /* Untouched SDK data, scratch banks, and core-1 placement. */
    /* by default we put core 0 stack at the end of scratch Y, so that if core 1
     * stack is not used then all of SCRATCH_X is free.
     */
    .stack_dummy (NOLOAD):
    {
        KEEP(*(.stack*))
    } > SCRATCH_Y
    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);
    __StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    __StackBottom = __StackTop - SIZEOF(.stack_dummy);
    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")
"""


class LinkerTests(unittest.TestCase):
    def fixture_hash(self, source=SOURCE):
        return patch.object(make_linker, "ORIGINAL_SHA256", hashlib.sha256(source).hexdigest())

    def test_stack_and_heap_cannot_overlap(self):
        with self.fixture_hash():
            generated = make_linker.generate(SOURCE).decode()
        self.assertIn("__HeapLimit = ORIGIN(RAM) + LENGTH(RAM) - 16384;", generated)
        self.assertIn(".stack_dummy __HeapLimit (NOLOAD)", generated)
        self.assertIn("KEEP(*(.stack*))\n    } > RAM", generated)
        self.assertIn("__StackLimit = __HeapLimit;", generated)
        self.assertIn("__StackTop = ORIGIN(RAM) + LENGTH(RAM);", generated)
        self.assertIn("ASSERT(__heap_min_end <= __HeapLimit", generated)
        self.assertIn("ASSERT(SIZEOF(.stack_dummy) == 16384", generated)
        self.assertIn("ASSERT(__StackBottom == __HeapLimit", generated)
        self.assertIn("/* Original SDK notice. */", generated)
        self.assertIn("/* Untouched SDK data, scratch banks, and core-1 placement. */", generated)
        self.assertEqual(0x20000000 + 512 * 1024 - make_linker.STACK_BYTES, 0x2007C000)

    def test_unknown_input_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unsupported SDK linker"):
            make_linker.generate(SOURCE)

    def test_missing_or_duplicate_context_rejected(self):
        for source in (SOURCE.replace(b"KEEP(*(.heap*))", b"changed heap"),
                       SOURCE + b"    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);\n"):
            with self.fixture_hash(source), self.assertRaisesRegex(ValueError, "Unexpected linker context"):
                make_linker.generate(source)

    def test_generates_outside_sdk_without_modifying_input(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sdk = root / "sdk"
            source = sdk / make_linker.SDK_MAP
            source.parent.mkdir(parents=True)
            source.write_bytes(SOURCE)
            output = root / "build" / "airpods.ld"
            with self.fixture_hash():
                self.assertEqual(make_linker.main(["--sdk", str(sdk), "--output", str(output)]), 0)
            self.assertEqual(source.read_bytes(), SOURCE)
            self.assertIn(b"core-0 stack must be 16 KiB", output.read_bytes())

    def test_bad_input_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sdk = root / "sdk"
            source = sdk / make_linker.SDK_MAP
            source.parent.mkdir(parents=True)
            source.write_bytes(b"unreviewed SDK map")
            output = root / "airpods.ld"
            output.write_bytes(b"previous layout")
            self.assertEqual(make_linker.main(["--sdk", str(sdk), "--output", str(output)]), 1)
            self.assertEqual(output.read_bytes(), b"previous layout")

    def test_will_not_write_inside_sdk(self):
        with tempfile.TemporaryDirectory() as temp:
            sdk = Path(temp)
            source = sdk / make_linker.SDK_MAP
            source.parent.mkdir(parents=True)
            source.write_bytes(SOURCE)
            with self.fixture_hash():
                self.assertEqual(make_linker.main(["--sdk", str(sdk), "--output", str(source)]), 1)
            self.assertEqual(source.read_bytes(), SOURCE)


if __name__ == "__main__":
    unittest.main()
