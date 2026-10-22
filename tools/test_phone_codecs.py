#!/usr/bin/env python3
"""Test the phone decoders against the pinned SDK's real SBC codec."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ARM_SUPPORT = """#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
extern char _end;
extern void probe_emit(const char *);
extern void _exit(int) __attribute__((noreturn));
void __assert_func(const char *file, int line, const char *function, const char *expression) {
    char text[512];
    snprintf(text, sizeof(text), "ASSERT %s:%d %s: %s\\n", file, line, function, expression);
    probe_emit(text); _exit(1);
}
void *_sbrk(ptrdiff_t n) {
    static char *next;
    if (!next) next = &_end;
    if (n < 0 || n > 65536 - (next - &_end)) { errno = ENOMEM; return (void *)-1; }
    char *old = next; next += n; return old;
}
int __wrap_printf(const char *format, ...) {
    char text[512]; va_list args; va_start(args, format);
    int count = vsnprintf(text, sizeof(text), format, args);
    va_end(args); probe_emit(text); return count;
}
int __wrap_puts(const char *text) { probe_emit(text); probe_emit("\\n"); return 0; }
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cc")
    parser.add_argument("--arm", action="store_true", help="functional ARM32 check in QEMU M7; no Pico timing")
    args = parser.parse_args()
    if args.arm and args.sanitize:
        parser.error("--sanitize is a native-only check")
    if args.arm and not shutil.which("qemu-system-arm"):
        parser.error("--arm requires qemu-system-arm")
    sdk = args.sdk.resolve()
    subprocess.run(["python3", str(ROOT / "tools/prepare_sdk.py"), "--sdk", str(sdk), "--check"], check=True)
    codec = sdk / "lib/btstack/3rd-party/bluedroid"
    command = [args.cc or ("arm-none-eabi-gcc" if args.arm else "cc"),
               "-std=c11", "-O3", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-unused-variable",
               "-Wno-unused-but-set-variable", "-Wno-missing-field-initializers"]
    if args.arm:
        command += ["-mcpu=cortex-m33", "-mthumb", "-march=armv8-m.main+fp+dsp",
                    "-mfloat-abi=softfp", "-ffunction-sections", "-fdata-sections", "-DPHONE_TEST_ARM"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-no-pie"]
    command += ["-I", str(ROOT / "3rd-party/libfreeaptx"),
                "-I", str(ROOT), "-I", str(sdk / "lib/btstack/src"),
                "-I", str(codec / "decoder/include"), "-I", str(codec / "encoder/include")]
    sources = [ROOT / "tests/phone_codec_test.c", ROOT / "tests/phone_resample_test.c",
               sdk / "lib/btstack/src/btstack_resample.c", ROOT / "src/btstack/phone_codec.c",
               ROOT / "3rd-party/libfreeaptx/freeaptx.c"]
    sources += sorted((codec / "decoder/srce").glob("*.c"))
    sources += sorted((codec / "encoder/srce").glob("*.c"))
    with tempfile.TemporaryDirectory(prefix="phone-codecs-") as temporary:
        if args.arm:
            support = Path(temporary) / "support.c"
            support.write_text(ARM_SUPPORT)
            sources += [support, ROOT / "tools/codec_probe/startup.c"]
        objects = []
        for index, source in enumerate(sources):
            obj = Path(temporary) / f"{index}.o"
            flags = command.copy()
            # Pinned SBC fixed-point code uses signed/zero-bit shifts intentionally.
            # Retain address/bounds checks there and full UB checks in our wrapper.
            if args.sanitize and source.is_relative_to(codec):
                flags += ["-fno-sanitize=shift"]
            subprocess.run(flags + ["-c", str(source), "-o", str(obj)],
                           check=True, timeout=30)
            objects.append(str(obj))
        binary = Path(temporary) / "test"
        linker = ["-nostartfiles", "-Wl,--gc-sections", "-Wl,--wrap=printf", "-Wl,--wrap=puts",
                  "-T", str(ROOT / "tools/codec_probe/probe.ld")] if args.arm else []
        linker += ["-Wl,--wrap=malloc", "-Wl,--wrap=calloc", "-Wl,--wrap=free"]
        libraries = ["-Wl,--start-group", "-lc", "-lm", "-lnosys", "-Wl,--end-group"] if args.arm else []
        subprocess.run(command + linker + objects + libraries + ["-o", str(binary)],
                       check=True, timeout=90)
        run = ["qemu-system-arm", "-M", "mps2-an500", "-nographic", "-semihosting-config",
               "enable=on,target=native", "-kernel", str(binary)] if args.arm else [str(binary)]
        subprocess.run(run, check=True, timeout=30)


if __name__ == "__main__":
    main()
