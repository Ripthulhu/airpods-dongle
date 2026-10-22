#!/usr/bin/env python3
"""Run the built ARM codec in QEMU; this is not a Pico performance benchmark."""
import argparse
import pathlib
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=pathlib.Path,
                        help="built Cortex-M33 libfdk-aac.a")
    parser.add_argument("--heap-bytes", required=True, type=int,
                        help="__StackLimit minus end from the firmware ELF")
    parser.add_argument("--output", type=pathlib.Path,
                        help="temporary build directory; defaults to build/codec-probe")
    args = parser.parse_args()
    directory = pathlib.Path(__file__).resolve().parent
    root = directory.parents[1]
    output = (args.output or root / "build" / "codec-probe").resolve()
    library = args.library.resolve()
    if not library.is_file():
        parser.error(f"codec library not found: {library}")
    if not 0 < args.heap_bytes <= 1024 * 1024:
        parser.error("--heap-bytes must be between 1 and 1048576")
    compiler = shutil.which("arm-none-eabi-gcc")
    emulator = shutil.which("qemu-system-arm")
    if compiler is None or emulator is None:
        parser.error("arm-none-eabi-gcc and qemu-system-arm are required")
    output.mkdir(parents=True, exist_ok=True)
    elf = output / "codec-probe.elf"
    fdk = root / "3rd-party" / "fdk-aac"
    command = [compiler, "-mcpu=cortex-m33", "-mthumb", "-march=armv8-m.main+fp+dsp",
               "-mfloat-abi=softfp", "-mcmse", "-O2", "-g", "-ffreestanding",
               "-Wall", "-Wextra", "-Werror", f"-DPROBE_HEAP_BYTES={args.heap_bytes}"]
    for module in ("libAACenc", "libAACdec", "libSYS"):
        command += ["-isystem", str(fdk / module / "include")]
    command += ["-nostartfiles", f"-Wl,-T,{directory / 'probe.ld'}",
                str(directory / "startup.c"), str(directory / "probe.c"), str(library),
                "-Wl,--start-group", "-lc", "-lm", "-lnosys", "-Wl,--end-group",
                "-o", str(elf)]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (output / "build.log").write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr, end="")
        return compiled.returncode
    result = subprocess.run([emulator, "-M", "mps2-an500", "-nographic",
                             "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(elf)],
                            capture_output=True, text=True, timeout=40)
    transcript = result.stdout + result.stderr
    (output / "result.log").write_text(transcript)
    print(transcript, end="")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
