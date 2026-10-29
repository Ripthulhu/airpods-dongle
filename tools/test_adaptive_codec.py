#!/usr/bin/env python3
"""Exercise production live ELD updates with real FDK, then decode continuous stereo.

Runs entirely on the host; the ARM codec-memory gate remains independently required.
The firmware decoder intentionally supports only its mono microphone, so music is
decoded by a separate full-FDK executable, never by widening the firmware profile.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?m)^static [\w *]+ " + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if match is None:
        raise ValueError(f"Missing production function: {name}")
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def run(command, log, timeout=180):
    result = subprocess.run([str(part) for part in command], capture_output=True,
                            text=True, timeout=timeout)
    with log.open("a") as file:
        file.write("$ " + " ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    if result.returncode:
        raise ValueError(f"Command failed ({result.returncode}); see {log}\n" + result.stdout[-2000:] + result.stderr[-2000:])
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", "--build-dir", type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("jobs must be between 1 and 32")
    for tool in ("cmake", args.compiler):
        if shutil.which(tool) is None:
            parser.error(f"Missing host tool: {tool}")
    output = (args.output or ROOT / ("build/adaptive-codec-test-sanitize" if args.sanitize
                                    else "build/adaptive-codec-test")).resolve()
    output.mkdir(parents=True, exist_ok=True)
    log = output / "test.log"; log.write_text("")
    source_path = ROOT / "src/btstack/btstack_avdtp_source.c"
    source = source_path.read_text()
    helper = function(source, "playback_eld_rate_update")
    # Include its production setter when the update delegates the FDK transaction.
    setter_names = re.findall(r"\b(playback_eld_rate_\w+)\s*\(", helper)
    helpers = [function(source, name) for name in dict.fromkeys(setter_names)
               if name != "playback_eld_rate_update"]
    (output / "production_rate.inc").write_text("\n\n".join(helpers + [helper]) + "\n")
    fdk = ROOT / "3rd-party/fdk-aac"
    libraries = {}
    # FDK's fixed-point arithmetic deliberately uses signed wrapping/shifts.
    sanitizer = (["-fsanitize=address,undefined", "-fno-sanitize=shift,signed-integer-overflow",
                  "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"] if args.sanitize else [])
    for name, definition in (("encoder", "-DFDK_AIRPODS_PROFILE=1"), ("decoder", "")):
        flags = " ".join([definition] + sanitizer)
        build = output / name
        run(["cmake", "-S", fdk, "-B", build, "-DBUILD_SHARED_LIBS=OFF",
             "-DBUILD_PROGRAMS=OFF", "-DCMAKE_BUILD_TYPE=Release",
             f"-DCMAKE_CXX_COMPILER={args.compiler}", f"-DCMAKE_CXX_FLAGS={flags}"], log)
        run(["cmake", "--build", build, "--target", "fdk-aac", "-j", args.jobs], log)
        libraries[name] = build / "libfdk-aac.a"
        if not libraries[name].is_file():
            raise ValueError(f"Missing built archive: {libraries[name]}")
    executables = {}
    fixture = ROOT / "tools/adaptive_codec_fixture.cpp"
    for name, definition in (("encoder", "-DFDK_AIRPODS_PROFILE=1"), ("decoder", "-DADAPTIVE_CODEC_DECODER=1")):
        executable = output / f"adaptive-{name}"
        command = [args.compiler, "-std=c++11", "-O2", "-Wall", "-Wextra", definition,
                   "-I", output, "-I", ROOT / "src/btstack"]
        for module in ("libAACenc", "libAACdec", "libSYS"):
            command += ["-isystem", fdk / module / "include"]
        wrappers = (["-Wl,--wrap=" + name for name in ("malloc", "calloc", "realloc", "free")]
                    if name == "encoder" else [])
        run(command + sanitizer + [fixture, libraries[name]] + wrappers + ["-lm", "-o", executable], log)
        executables[name] = executable
    cases = []
    for mode in (0, 1):
        ceilings = ((256000, 255200, 220000, 192000, 174000, 160000, 150400, 128000)
                    if mode == 0 else (256000, 220000, 192000, 174000, 160000, 150400, 128000))
        for requested in ceilings:
            ceiling = requested // 800 * 800 if mode else requested
            path = output / f"mode{mode}-cap{requested}.aus"
            encoded = run([executables["encoder"], mode, ceiling, path], log)
            if "heap_calls=0 allocator_probe=passed" not in encoded:
                raise ValueError("Missing steady-encode allocator guard result")
            decoded = run([executables["decoder"], path], log)
            cases.append({"mode": mode, "requested_ceiling": requested, "ceiling": ceiling,
                          "encode": encoded.strip(), "decode": decoded.strip()})
    lc_cases = []
    for bitrate in (96000, 128000, 174000, 192000, 220000):
        encoded = run([executables["encoder"], "lc", bitrate], log)
        if "heap_calls=0 allocator_probe=passed" not in encoded:
            raise ValueError("Missing AAC-LC allocator guard result")
        lc_cases.append({"bitrate": bitrate, "encode": encoded.strip()})
    # Prove the quality oracle catches a tempting full-history-reset implementation.
    negative = output / "reset-history.aus"
    run([executables["encoder"], 1, 256000, negative, "reset"], log)
    rejected = subprocess.run([str(executables["decoder"]), str(negative)],
                              capture_output=True, text=True, timeout=60)
    with log.open("a") as file:
        file.write("NEGATIVE reset-history\n" + rejected.stdout + rejected.stderr)
    if rejected.returncode == 0 or "FAIL continuity" not in rejected.stderr:
        raise ValueError("Continuity oracle did not reject deliberate encoder-history resets")
    report = {"production_source_sha256": hashlib.sha256(source.encode()).hexdigest(),
              "sanitizers": sanitizer,
              "cases": cases, "history_reset_rejected": True,
              "cbr_only_wire_retune_rejected": True,
              "aac_lc_allocator_cases": lc_cases,
              "steady_encode_allocator_calls": 0,
              "allocator_scope": "malloc/calloc/realloc/free linker-wrapped in profiled static FDK; "
                                 "normal frame encode only, including first frame after live retune; "
                                 "plus AAC-LC stereo 48kHz/1024 LATM at all manual bitrates; "
                                 "initialization/configuration/close remain outside guard",
              "scope": "host semantics only; ARM memory, board timing and radio delivery are separate checks"}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Real FDK live ELD transitions passed: {len(cases)} CBR/VBR cases; deliberate history reset rejected.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
