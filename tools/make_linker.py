#!/usr/bin/env python3
"""Reserve a 16 KiB core-0 stack using the pinned SDK linker layout."""

import argparse
import hashlib
from pathlib import Path
import sys

SDK_MAP = Path("src/rp2_common/pico_crt0/rp2350/memmap_default.ld")
ORIGINAL_SHA256 = "2349d98352370fc22d84d482e53313300e8a52ea42529eced235fa99c109234f"
STACK_BYTES = 16384


def generate(source):
    if hashlib.sha256(source).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Unsupported SDK linker script; expected the original Pico SDK 2.1.1 RP2350 map")
    text = source.decode("utf-8")
    replacements = (
        ("        KEEP(*(.heap*))\n", "        KEEP(*(.heap*))\n        __heap_min_end = .;\n"),
        ("    __HeapLimit = ORIGIN(RAM) + LENGTH(RAM);",
         f"    __HeapLimit = ORIGIN(RAM) + LENGTH(RAM) - {STACK_BYTES};"),
        ("    /* by default we put core 0 stack at the end of scratch Y, so that if core 1\n"
         "     * stack is not used then all of SCRATCH_X is free.\n     */",
         "    /* Codec calls need a larger core-0 stack, reserved above the heap. */"),
        ("    .stack_dummy (NOLOAD):\n    {\n        KEEP(*(.stack*))\n    } > SCRATCH_Y",
         "    .stack_dummy __HeapLimit (NOLOAD):\n    {\n        KEEP(*(.stack*))\n    } > RAM"),
        ("    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);", "    __StackLimit = __HeapLimit;"),
        ("    __StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);",
         "    __StackTop = ORIGIN(RAM) + LENGTH(RAM);"),
        ('    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")',
         '    ASSERT(__heap_min_end <= __HeapLimit, "data and minimum heap overlap core-0 stack")\n'
         f'    ASSERT(SIZEOF(.stack_dummy) == {STACK_BYTES}, "core-0 stack must be 16 KiB")\n'
         '    ASSERT(__StackBottom == __HeapLimit, "core-0 stack boundary mismatch")'),
    )
    for before, after in replacements:
        if text.count(before) != 1:
            raise ValueError("Unexpected linker context; refusing to generate an unverified layout")
        text = text.replace(before, after)
    return ("/* Generated from the original Pico SDK 2.1.1 RP2350 linker script. */\n" + text).encode("utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        sdk = args.sdk.resolve()
        output = args.output.resolve()
        if output.is_relative_to(sdk):
            raise ValueError("The generated linker script must be outside the SDK checkout")
        result = generate((sdk / SDK_MAP).read_bytes())
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(result)
    except (ValueError, OSError) as error:
        print(f"Linker generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
