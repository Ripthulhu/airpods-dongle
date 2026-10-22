#!/usr/bin/env python3
"""Run host regression tests against the production protocol and buffer code."""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    tests = {
        "peer": ["tests/peer_test.c"],
        "button_gesture": ["tests/button_gesture_test.c"],
        "slot": ["-Itests/slot_stubs", "tests/slot_test.c"],
        "hci": ["-Itests/hci_stubs", "tests/hci_test.c"],
        "ble_scan": ["-Itests/ble_scan_stubs", "tests/ble_scan_test.c"],
        "playback": ["tests/playback_test.c"],
        "aacp": ["-Itests/aacp_stubs", "tests/aacp_test.c"],
        "usb_control": ["tests/usb_control_test.c"],
        "mic_decoder": ["-Itests/mic_stubs", "-Wno-deprecated-declarations",
                        "tests/mic_decoder_test.c", "-lm"],
        "mic_worker_thread": ["-Itests/mic_stubs", "-Wno-deprecated-declarations",
                              "tests/mic_worker_thread_test.c", "-lm", "-pthread"],
        "media_control": ["-Itests/media_stubs", "tests/media_control_test.c",
                          "src/tinyusb/media_control.c"],
        "debug_cdc": ["-Itests/cdc_stubs", "tests/debug_cdc_test.c"],
    }
    with tempfile.TemporaryDirectory(prefix="airpods-tests-") as tmp:
        for name, sources in tests.items():
            binary = pathlib.Path(tmp) / name
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1"]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
            subprocess.run([*command, *sources, "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True)
            print(f"PASS {name}", flush=True)
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"],
                   cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
