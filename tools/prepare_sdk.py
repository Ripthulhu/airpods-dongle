#!/usr/bin/env python3
"""Fetch the pinned SDK and apply only the reviewed transport/USB patches."""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SDK_REVISION = "079c6f39023649b154152db30f1d781e884879bc"
SUBMODULES = ("btstack", "tinyusb", "cyw43-driver", "mbedtls", "lwip")


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(sdk, apply=False):
    if git(sdk, "rev-parse", "HEAD") != SDK_REVISION:
        raise ValueError("Unsupported pico-sdk revision; use the pinned SDK 2.3.1 checkout")
    manifest = json.loads((ROOT / "patches" / "sdk.json").read_text())
    for name in SUBMODULES:
        expected = git(sdk, "rev-parse", f"HEAD:lib/{name}")
        if git(sdk / "lib" / name, "rev-parse", "HEAD") != expected:
            raise ValueError(f"Unsupported SDK submodule revision: {name}")
    # Validate every input before changing any checkout.
    pending = []
    for library, spec in manifest.items():
        base = sdk if library == "sdk" else sdk / "lib" / library
        hashes = {name: sha256(base / name) for name in spec["original"]}
        if hashes == spec["patched"]:
            continue
        if hashes != spec["original"]:
            raise ValueError(f"Unknown {library} patch state; refusing to overwrite local changes")
        if not apply:
            raise ValueError(f"Missing {library} patch; run tools/prepare_sdk.py --sdk {sdk}")
        patch = ROOT / "patches" / spec["patch"]
        git(base, "apply", "--check", str(patch))
        pending.append((base, patch, spec))
    for base, patch, spec in pending:
        git(base, "apply", str(patch))
        if any(sha256(base / name) != digest for name, digest in spec["patched"].items()):
            raise ValueError(f"Patch readback failed: {base}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=pathlib.Path, required=True)
    parser.add_argument("--check", action="store_true", help="validate without modifying files")
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    try:
        if not sdk.exists() and not args.check:
            subprocess.run(["git", "-c", "core.autocrlf=false", "clone", "--branch", "2.3.1", "--depth", "1",
                            "https://github.com/raspberrypi/pico-sdk.git", str(sdk)], check=True)
        if not args.check:
            if git(sdk, "rev-parse", "HEAD") != SDK_REVISION:
                raise ValueError("Unsupported pico-sdk revision")
            subprocess.run(["git", "-c", "core.autocrlf=false", "-C", str(sdk), "submodule", "update", "--init", "--depth", "1",
                            *(f"lib/{name}" for name in SUBMODULES)], check=True)
        check(sdk, apply=not args.check)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"SDK check failed: {error}", file=sys.stderr)
        return 1
    print("SDK 2.3.1 revisions and patch readback verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
