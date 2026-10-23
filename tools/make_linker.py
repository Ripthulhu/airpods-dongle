#!/usr/bin/env python3
"""Generate checked 16 KiB core-0 stack overrides for the pinned SDK layout."""
import argparse
import hashlib
from pathlib import Path
import sys

SDK_INCLUDE = Path("src/rp2_common/pico_standard_link/script_include")
ORIGINAL_SHA256 = {
    "section_heap.incl": "e26b5a5c39106dc8e5e0d1ab1f4a9074499a18e02823982cb34b24de96aa1615",
    "sections_stack.incl": "5ebe190744b4f5476c525f06a5f48fdb06c192651a94825e261a47c6c09f4b77",
    "section_end.incl": "cb2a6d74dfbf0fedd3452e05742e46791a00f96389ca449f8954be3219027f44"
}
LAYOUT_SHA256 = {
    "src/rp2350/pico_platform/memmap_default.ld": "aaea13e8414aa47f07d42c25cef379a92a5cd6494688023e83f43d5a71e1f8ab",
    "src/rp2350/pico_platform/script_include/default_locations.ld": "c2f590a425fc33132190505e65d0a0bcf2b2f98d2718a9d2a5c8d6d7b48e5957",
    "src/rp2_common/pico_standard_link/script_include/memmap_default.incl": "d953f5254e28b2b48a3b1b375f8fe88e4c229c6d1385d90a04ec8db2451eef38",
    "src/rp2_common/pico_standard_link/script_include/set_memory_locations.incl": "cab80259f02897368ba774cbbf0c7e5437ed280e9c8a4ec90427d546911655ff",
    "src/rp2_common/pico_standard_link/script_include/memory_ram.incl": "7db6037823e36336eca6edbe17689e544f6427d3ee7bf91f4ee997f94a6e78ea",
    "src/rp2_common/pico_standard_link/script_include/memory_scratch.incl": "1af36bd27a98b988b42ccc3b676d4310ed30fa92efd64a410d76830901786311",
    "src/rp2_common/pico_standard_link/script_include/sections_default.incl": "9e8ae86fb153df5bf51d54cc1404a8423abad1395e1d1a3a46e9757fbcaf1268",
    "src/rp2_common/pico_standard_link/script_include/sections_scratch.incl": "8e8be615e496c14fe6d06aee55e4f54a45d43e7d20eabbe596f725f738d616c9",
}
STACK_BYTES = 16384

def generate(sources):
    if set(sources) != set(ORIGINAL_SHA256) or any(
        hashlib.sha256(sources[name]).hexdigest() != digest
        for name, digest in ORIGINAL_SHA256.items()
    ):
        raise ValueError("Unsupported SDK linker input; expected Pico SDK 2.3.1 includes")
    replacements = {
        "section_heap.incl": (
            ("        __end__ = .;", "        . = ALIGN(8);\n        __end__ = .;"),
            ("        KEEP(*(.heap*))", "        KEEP(*(.heap*))\n        __heap_min_end = .;"),
            ("    __HeapLimit = DEFINED(HEAP_LIMIT) ? HEAP_LIMIT : ORIGIN(RAM) + LENGTH(RAM);", "    __HeapLimit = ORIGIN(RAM) + LENGTH(RAM) - 16384;"),
        ),
        "sections_stack.incl": (
            ("    .stack_dummy (NOLOAD):", "    .stack_dummy __HeapLimit (NOLOAD):"),
            ("        KEEP(*(.stack*))\n    } > SCRATCH_Y", "        KEEP(*(.stack*))\n    } > RAM"),
        ),
        "section_end.incl": (
            ("    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);", "    __StackLimit = __HeapLimit;"),
            ("    __StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);", "    __StackTop = ORIGIN(RAM) + LENGTH(RAM);"),
            ('    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")', '    ASSERT(__heap_min_end <= __HeapLimit, "data and minimum heap overlap core-0 stack")\n    ASSERT(SIZEOF(.stack_dummy) == 16384, "core-0 stack must be 16 KiB")\n    ASSERT(__StackBottom == __HeapLimit, "core-0 stack boundary mismatch")'),
        ),
    }
    outputs = {}
    for name, pairs in replacements.items():
        text = sources[name].decode("utf-8")
        for before, after in pairs:
            if text.count(before) != 1:
                raise ValueError("Unexpected linker context; refusing an unverified layout")
            text = text.replace(before, after)
        outputs[name] = ("/* Generated from Pico SDK 2.3.1; core-0 stack is in main RAM. */\n" + text).encode()
    return outputs

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="output override directory")
    args = parser.parse_args(argv)
    try:
        sdk, output = args.sdk.resolve(), args.output.resolve()
        if output.is_relative_to(sdk):
            raise ValueError("Generated linker overrides must be outside the SDK checkout")
        layout = {name: (sdk / name).read_bytes() for name in LAYOUT_SHA256}
        if any(hashlib.sha256(layout[name]).hexdigest() != digest
               for name, digest in LAYOUT_SHA256.items()):
            raise ValueError("Unsupported SDK linker base layout; expected Pico SDK 2.3.1")
        outputs = generate({name: (sdk / SDK_INCLUDE / name).read_bytes() for name in ORIGINAL_SHA256})
        targets = {name: (output / name).resolve() for name in outputs}
        for name, target in targets.items():
            if target != output / name or target.is_relative_to(sdk):
                raise ValueError("Generated linker output must be a direct file outside the SDK checkout")
            if target.exists() and not target.is_file():
                raise ValueError("Generated linker output target is not a file")
        output.mkdir(parents=True, exist_ok=True)
        for name, data in outputs.items():
            targets[name].write_bytes(data)
    except (ValueError, OSError, RuntimeError) as error:
        print(f"Linker generation failed: {error}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
