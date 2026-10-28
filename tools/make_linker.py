#!/usr/bin/env python3
"""Generate checked stack and shared-codec RAM overrides for the pinned SDK."""
import argparse
import hashlib
from pathlib import Path
import sys

SDK_INCLUDE = Path("src/rp2_common/pico_standard_link/script_include")
ORIGINAL_SHA256 = {
    "section_heap.incl": "e26b5a5c39106dc8e5e0d1ab1f4a9074499a18e02823982cb34b24de96aa1615",
    "sections_stack.incl": "5ebe190744b4f5476c525f06a5f48fdb06c192651a94825e261a47c6c09f4b77",
    "section_end.incl": "cb2a6d74dfbf0fedd3452e05742e46791a00f96389ca449f8954be3219027f44",
    "default_text_excludes.incl": "a6f4d599b24017fadfe0ae354216942b714f5c1425ad41f9c446af117ca92c1b",
    "default_rodata_excludes.incl": "206a030a8c8a67fad41a6ac14416f437bdfb5ae58b24420d8f7deb155fdd8bd8",
}
LAYOUT_SHA256 = {
    "src/rp2350/pico_platform/memmap_default.ld": "aaea13e8414aa47f07d42c25cef379a92a5cd6494688023e83f43d5a71e1f8ab",
    "src/rp2350/pico_platform/script_include/default_locations.ld": "c2f590a425fc33132190505e65d0a0bcf2b2f98d2718a9d2a5c8d6d7b48e5957",
    "src/rp2_common/pico_standard_link/script_include/memmap_default.incl": "d953f5254e28b2b48a3b1b375f8fe88e4c229c6d1385d90a04ec8db2451eef38",
    "src/rp2_common/pico_standard_link/script_include/set_memory_locations.incl": "cab80259f02897368ba774cbbf0c7e5437ed280e9c8a4ec90427d546911655ff",
    "src/rp2_common/pico_standard_link/script_include/memory_ram.incl": "7db6037823e36336eca6edbe17689e544f6427d3ee7bf91f4ee997f94a6e78ea",
    "src/rp2_common/pico_standard_link/script_include/memory_scratch.incl": "1af36bd27a98b988b42ccc3b676d4310ed30fa92efd64a410d76830901786311",
    "src/rp2_common/pico_standard_link/script_include/sections_default.incl": "9e8ae86fb153df5bf51d54cc1404a8423abad1395e1d1a3a46e9757fbcaf1268",
    "src/rp2_common/pico_standard_link/script_include/section_default_text.incl": "ee312d5f55332bdcd7ccac63fff29d7db9c03b5c0d861f9ae2452ff59435a3e1",
    "src/rp2_common/pico_standard_link/script_include/section_default_data.incl": "e58230564224cd26e04343ba9234f7f423cf1bcebd8e6039e59570c57aa9bb32",
    "src/rp2_common/pico_standard_link/script_include/section_default_rodata.incl": "762d99062e8ad9db886bd57b7fe9486899747d720326fff50648962aab03b2f3",
    "src/rp2_common/pico_standard_link/script_include/sections_scratch.incl": "8e8be615e496c14fe6d06aee55e4f54a45d43e7d20eabbe596f725f738d616c9",
}
STACK_BYTES = 16384
CODEC_RAM_OBJECTS = ("dct.cpp.o", "fft.cpp.o", "fft_rad2.cpp.o",
                     "mdct.cpp.o", "scale.cpp.o", "fixpoint_math.cpp.o",
                     "quantize.cpp.o", "bit_cnt.cpp.o", "dyn_bits.cpp.o", "aacenc_tns.cpp.o")

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
        # The SDK copies excluded text with .data at boot. Both codec cores use
        # these kernels; keep them out of their shared, contended flash cache.
        "default_text_excludes.incl": (
            ("*(EXCLUDE_FILE(*libgcc.a: *libm.a:) .text*)",
             "*(EXCLUDE_FILE(*libgcc.a: *libm.a: " +
             " ".join("*libfdk-aac.a:" + name for name in CODEC_RAM_OBJECTS) +
             ") .text*)"),
        ),
        # Quantization and bit counting repeatedly look up these encoder tables.
        "default_rodata_excludes.incl": (
            ("*(EXCLUDE_FILE(*libgcc.a: *libm.a:) .rodata*)",
             "*(EXCLUDE_FILE(*libgcc.a: *libm.a: *libfdk-aac.a:aacEnc_rom.cpp.o) .rodata*)"),
        ),
    }
    outputs = {}
    for name, pairs in replacements.items():
        text = sources[name].decode("utf-8")
        for before, after in pairs:
            if text.count(before) != 1:
                raise ValueError("Unexpected linker context; refusing an unverified layout")
            text = text.replace(before, after)
        outputs[name] = ("/* Generated from Pico SDK 2.3.1; stack and shared codec kernels use main RAM. */\n" + text).encode()
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
