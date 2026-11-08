"""Package only checked firmware and its exact committed source; never access hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
CONFIGURATION = {"PICO_BOARD": "pico2_w", "CMAKE_BUILD_TYPE": "Release", "AIRPODS_USB_FLOAT": "ON",
                 "AIRPODS_EXPERIMENTAL_RADIO0076": "ON", "AIRPODS_ENABLE_BLUETOOTH_CALLS": "OFF",
                 "AACELD_CBR_BANDWIDTH": "20000"}
FIRMWARE = {"airpods_dongle.uf2", "airpods_dongle.bin", "airpods_dongle.elf", "airpods_dongle.elf.map", "memory-check.json"}
PAYLOADS = FIRMWARE | {"COMMIT.txt", "COMPILER.txt", "BUILD.json", "SOURCES.json", "source.tar.gz", "airpods-cli.zip", "THIRD-PARTY-NOTICES.txt"}
SDK_NOTICES = ("LICENSE.TXT", "src/rp2_common/cmsis/stub/CMSIS/LICENSE.txt", "src/rp2_common/pico_printf/LICENSE",
               "src/rp2_common/pico_btstack/LICENSE.RP", "lib/btstack/LICENSE", "lib/tinyusb/LICENSE",
               "lib/cyw43-driver/LICENSE", "lib/cyw43-driver/LICENSE.RP")
SDK_HEADER_NOTICES = ("lib/btstack/3rd-party/bluedroid/encoder/include/sbc_encoder.h",
                      "lib/btstack/3rd-party/bluedroid/decoder/include/oi_codec_sbc.h",
                      "lib/btstack/3rd-party/rijndael/rijndael.h")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def version(root):
    match = re.search(r"(?m)^project\(airpods_dongle VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES ", (root / "CMakeLists.txt").read_text())
    require(match is not None, "Cannot identify firmware version")
    return match[1]


def cache_values(path):
    values = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r"([^/#][^:=]*):[^=]+=(.*)", line)
        if match:
            require(match[1] not in values, "Duplicate CMake cache setting")
            values[match[1]] = match[2]
    return values


def validate_images(uf2, binary):
    require(uf2 and len(uf2) % 512 == 0 and binary, "Malformed or empty firmware image")
    main = []
    for offset in range(0, len(uf2), 512):
        m0, m1, flags, address, length, block, count, family = struct.unpack_from("<8I", uf2, offset)
        require((m0, m1, length) == (0x0a324655, 0x9e5d5157, 256) and
                struct.unpack_from("<I", uf2, offset + 508)[0] == 0x0ab16f30, "Malformed UF2 block")
        payload = uf2[offset + 32:offset + 288]
        if family == 0xe48bff57:
            require(offset == 0 and flags == 0xa000 and address == 0x10ffff00 and block == 0 and count == 2 and
                    payload == bytes([0xef]) * 256, "Unexpected RP2350 compatibility block")
            continue
        require(family == 0xe48bff59 and flags == 0x2000 and
                0x10000000 <= address < address + 256 <= 0x103f0000, "UF2 family/address could overwrite pairing storage")
        require(address == 0x10000000 + len(main) * 256 and block == len(main), "Noncontiguous or duplicate UF2 block")
        main.append((count, payload))
    require(main and all(count == len(main) for count, _ in main), "Incomplete UF2 image")
    data = b"".join(payload for _, payload in main)
    require(len(binary) <= len(data) < len(binary) + 256 and data[:len(binary)] == binary and
            not any(data[len(binary):]), "UF2 payload does not match BIN")


def validate_memory(directory):
    memory = json.loads((directory / "memory-check.json").read_text())
    require(memory.get("passed") is True and memory.get("hardware_tested") is False, "Offline memory gate did not pass")
    require(memory.get("firmware_sha256") == digest(directory / "airpods_dongle.elf") and
            memory.get("uf2_sha256") == digest(directory / "airpods_dongle.uf2"), "Memory receipt does not bind these firmware images")
    require(memory.get("runtime_reserve_bytes") == 65536 and memory.get("remaining_after_peak_bytes", 0) >= 65536 and
            memory.get("fdk_pcm_bits") == 32 and memory.get("eld_cbr_bandwidth_hz") == 20000 and
            memory.get("usb_audio_mode") == "float", "Wrong codec format/bandwidth or missing 64 KiB reserve")
    validate_images((directory / "airpods_dongle.uf2").read_bytes(), (directory / "airpods_dongle.bin").read_bytes())
    return memory


def notices(root, sdk):
    paths = git(root, "ls-files").splitlines()
    tracked = [name for name in paths if Path(name).name.upper().startswith(("LICENSE", "COPYING", "NOTICE"))]
    require("src/radio0076/LICENSE.vendor.txt" in tracked, "Radio vendor license is missing")
    sections = [(name, (root / name).read_text(encoding="utf-8")) for name in tracked]
    sections += [("pico-sdk/" + name, (sdk / name).read_text(encoding="utf-8")) for name in SDK_NOTICES]
    for name in SDK_HEADER_NOTICES:
        header = (sdk / name).read_text(encoding="utf-8")
        match = re.match(r"\s*(/\*.*?\*/|(?://[^\n]*\n)+)", header, re.S)
        require(match and re.search("copyright|public domain", match[0], re.I), f"Missing codec copyright notice: {name}")
        sections.append(("pico-sdk/" + name + " (header notice)", match[0]))
    return "Third-party notices; original terms are retained below. Source and dependency revisions are in SOURCES.json.\n\n" + "\n\n".join(f"===== {name} =====\n{text}" for name, text in sections)


def package(root, build, sdk):
    require(not git(root, "status", "--porcelain", "--untracked-files=all"), "Package only a clean committed checkout")
    commit, release_version = git(root, "rev-parse", "HEAD"), version(root)
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "Invalid source commit")
    cache = cache_values(build / "CMakeCache.txt")
    require(all(cache.get(key) == value for key, value in CONFIGURATION.items()), "Build is not the unified Release/.0076/PCM32 configuration")
    require(Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() == root.resolve() and
            Path(cache["PICO_SDK_PATH"]).resolve() == sdk.resolve(), "CMake build belongs to a different source/SDK")
    sources = json.loads((root / "SOURCES.json").read_text())
    require(git(sdk, "rev-parse", "HEAD") == sources["pico_sdk"]["revision"], "SDK revision differs from pinned source")
    output = build / "artifact"
    output.mkdir(exist_ok=True)
    require(not any(output.iterdir()) and not output.is_symlink(), "Refusing to overwrite an existing artifact directory")
    for name in FIRMWARE:
        source = build / ("codec-probe/memory-check.json" if name == "memory-check.json" else name)
        require(source.is_file() and not source.is_symlink(), f"Missing regular build output: {name}")
        shutil.copyfile(source, output / name)
    memory = validate_memory(output)
    (output / "COMMIT.txt").write_text(commit + "\n")
    (output / "COMPILER.txt").write_bytes(subprocess.check_output([cache["CMAKE_C_COMPILER"], "--version"]))
    shutil.copyfile(root / "SOURCES.json", output / "SOURCES.json")
    (output / "THIRD-PARTY-NOTICES.txt").write_text(notices(root, sdk), encoding="utf-8")
    with (output / "source.tar.gz").open("wb") as archive:
        subprocess.run(["git", "-C", str(root), "archive", "--format=tar.gz", "--prefix=airpods-dongle/", commit], stdout=archive, check=True)
    cli_files = ("tools/airpods.py", "tools/audio_diagnostics.py", "docs/CLI.md", "LICENSE", "LICENSE.Apache-2.0")
    with zipfile.ZipFile(output / "airpods-cli.zip", "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in cli_files:
            archive.writestr(zipfile.ZipInfo("airpods-cli/" + name), (root / name).read_bytes(), compress_type=zipfile.ZIP_DEFLATED)
        archive.writestr(zipfile.ZipInfo("airpods-cli/README.md"), "# AirPods command line\n\nPython 3.10+, no extra packages. From this directory run `python tools/airpods.py help`. See docs/CLI.md.\n")
        archive.writestr(zipfile.ZipInfo("airpods-cli/COMMIT.txt"), commit + "\n")
    frozen = json.loads((root / "src/radio0076/frozen.json").read_text())
    require(all(digest(root / "src/radio0076" / name) == value for name, value in frozen.items()), "Frozen radio inputs changed")
    receipt = dict(commit=commit, version=release_version, configuration={key: cache[key] for key in CONFIGURATION},
                   sdk_revision=git(sdk, "rev-parse", "HEAD"), sdk_patch_sha256=digest(root / "patches/sdk.json"),
                   radio=dict(revision=".0076", frozen_inputs=frozen), hardware_tested=False,
                   runtime_reserve_bytes=memory["runtime_reserve_bytes"], remaining_after_peak_bytes=memory["remaining_after_peak_bytes"],
                   files={path.name: digest(path) for path in sorted(output.iterdir())})
    (output / "BUILD.json").write_text(json.dumps(receipt, indent=2) + "\n")
    (output / "SHA256SUMS").write_text("".join(f"{digest(output / name)}  {name}\n" for name in sorted(PAYLOADS)))
    require(not git(root, "status", "--porcelain", "--untracked-files=all"), "Source checkout changed during packaging")
    validate_package(output, commit, release_version)
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as stream:
            stream.write(f"commit={commit}\nversion={release_version}\nrelease_tag=v{release_version}-{commit[:12]}\n")
    print(f"Packaged unified firmware {release_version} at {commit}")


def validate_package(directory, commit, release_version):
    require({path.name for path in directory.iterdir()} == PAYLOADS | {"SHA256SUMS"}, "Unexpected or missing release asset")
    require(all(path.is_file() and not path.is_symlink() for path in directory.iterdir()), "Release assets must be regular files")
    hashes = {name: digest(directory / name) for name in PAYLOADS}
    checksums = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/\\]+)", line)
        require(match and match[2] in PAYLOADS and match[2] not in checksums, "Malformed or duplicate release checksum")
        checksums[match[2]] = match[1]
    require(checksums == hashes, "Release checksums differ from payload")
    receipt = json.loads((directory / "BUILD.json").read_text())
    require(receipt["commit"] == (directory / "COMMIT.txt").read_text().strip() == commit and receipt["version"] == release_version,
            "Release source/version does not match the build")
    require(receipt["configuration"] == CONFIGURATION and receipt["hardware_tested"] is False, "Unexpected release configuration")
    require(receipt["files"] == {key: value for key, value in hashes.items() if key != "BUILD.json"}, "Build receipt does not bind complete payload")
    validate_memory(directory)
    return {name: (digest(directory / name), (directory / name).stat().st_size) for name in PAYLOADS | {"SHA256SUMS"}}


def self_test():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / "CMakeLists.txt").write_text("project(airpods_dongle VERSION 0.1.62 LANGUAGES C CXX ASM)\n")
        assert version(root) == "0.1.62"
        (root / "CMakeCache.txt").write_text("PICO_BOARD:STRING=pico2_w\nAIRPODS_USB_FLOAT:BOOL=ON\n")
        assert cache_values(root / "CMakeCache.txt")["PICO_BOARD"] == "pico2_w"
        binary = b"firmware fixture"
        block = bytearray(512)
        struct.pack_into("<8I", block, 0, 0x0a324655, 0x9e5d5157, 0x2000, 0x10000000, 256, 0, 1, 0xe48bff59)
        block[32:32 + len(binary)] = binary
        struct.pack_into("<I", block, 508, 0x0ab16f30)
        validate_images(bytes(block), binary)
        for offset, value in ((0, 0), (12, 0x103f0000), (28, 0xe48bff56)):
            bad = bytearray(block)
            struct.pack_into("<I", bad, offset, value)
            try:
                validate_images(bytes(bad), binary)
            except ValueError:
                pass
            else:
                raise AssertionError("Unsafe UF2 accepted")
        (root / "CMakeLists.txt").unlink()
        (root / "CMakeCache.txt").unlink()
        commit = "a" * 40
        for name in PAYLOADS:
            (root / name).write_bytes(b"release fixture")
        (root / "airpods_dongle.uf2").write_bytes(block)
        (root / "airpods_dongle.bin").write_bytes(binary)
        (root / "COMMIT.txt").write_text(commit + "\n")
        memory = dict(passed=True, hardware_tested=False, firmware_sha256=digest(root / "airpods_dongle.elf"),
                      uf2_sha256=digest(root / "airpods_dongle.uf2"), runtime_reserve_bytes=65536,
                      remaining_after_peak_bytes=65880, fdk_pcm_bits=32, eld_cbr_bandwidth_hz=20000, usb_audio_mode="float")
        (root / "memory-check.json").write_text(json.dumps(memory))
        def seal():
            receipt = dict(commit=commit, version="0.1.62", configuration=CONFIGURATION, hardware_tested=False,
                           files={name: digest(root / name) for name in PAYLOADS if name != "BUILD.json"})
            (root / "BUILD.json").write_text(json.dumps(receipt))
            (root / "SHA256SUMS").write_text("".join(f"{digest(root / name)}  {name}\n" for name in sorted(PAYLOADS)))
        seal()
        assert len(validate_package(root, commit, "0.1.62")) == 13
        for changes in ({"commit": "b" * 40}, {"version": "0.1.61"},
                        {"configuration": {**CONFIGURATION, "AIRPODS_EXPERIMENTAL_RADIO0076": "OFF"}}):
            saved = json.loads((root / "BUILD.json").read_text())
            (root / "BUILD.json").write_text(json.dumps({**saved, **changes}))
            (root / "SHA256SUMS").write_text("".join(f"{digest(root / name)}  {name}\n" for name in sorted(PAYLOADS)))
            try:
                validate_package(root, commit, "0.1.62")
            except ValueError:
                pass
            else:
                raise AssertionError("Wrong release source/configuration accepted")
            seal()
        for mutate in (lambda: (root / "COMMIT.txt").write_text("b" * 40),
                       lambda: (root / "SHA256SUMS").write_text("../private.key"),
                       lambda: (root / "airpods_dongle.uf2").write_bytes(b"altered")):
            mutate()
            try:
                validate_package(root, commit, "0.1.62")
            except ValueError:
                pass
            else:
                raise AssertionError("Corrupt release accepted")
            (root / "COMMIT.txt").write_text(commit + "\n")
            (root / "airpods_dongle.uf2").write_bytes(block)
            seal()
        memory["remaining_after_peak_bytes"] = 65535
        (root / "memory-check.json").write_text(json.dumps(memory))
        seal()
        try:
            validate_package(root, commit, "0.1.62")
        except ValueError:
            pass
        else:
            raise AssertionError("Insufficient runtime reserve accepted")
        # The archive must come from Git, never the working tree or ignored build/private files.
        source = root / "source"
        source.mkdir()
        subprocess.run(["git", "init", "-q", str(source)], check=True)
        (source / "tracked.c").write_text("source")
        (source / ".gitignore").write_text("private.key\n")
        (source / "private.key").write_text("never archive")
        subprocess.run(["git", "-C", str(source), "add", "."], check=True)
        subprocess.run(["git", "-C", str(source), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                        "-c", "commit.gpgsign=false", "commit", "-qm", "fixture"], check=True)
        import io, tarfile
        archived = subprocess.check_output(["git", "-C", str(source), "archive", "HEAD"])
        with tarfile.open(fileobj=io.BytesIO(archived)) as archive:
            assert set(archive.getnames()) == {"tracked.c", ".gitignore"}
        with zipfile.ZipFile(root / "cli.zip", "w") as archive:
            for name in ("tools/airpods.py", "tools/audio_diagnostics.py"):
                archive.write(ROOT / name, "airpods-cli/" + name)
        with zipfile.ZipFile(root / "cli.zip") as archive:
            archive.extractall(root / "extracted")
        cli = str(root / "extracted/airpods-cli/tools/airpods.py")
        help_text = subprocess.check_output([sys.executable, cli, "help"], text=True)
        require("settings" in help_text, "Packaged CLI help failed")
        for command in ("pair", "connect", "disconnect", "reconnect-policy"):
            require(command in help_text, f"Packaged CLI is missing {command}")
            command_help = subprocess.check_output([sys.executable, cli, "help", command], text=True)
            require(f" {command}" in command_help, f"Packaged CLI help failed for {command}")
    print("Firmware packaging checks passed")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
    else:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("--build", type=Path, default=ROOT / "build")
        parser.add_argument("--sdk", type=Path, required=True)
        arguments = parser.parse_args()
        package(ROOT, arguments.build.resolve(), arguments.sdk.resolve())
