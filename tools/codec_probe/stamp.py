#!/usr/bin/env python3
"""Retain the exact codec archive hash in the firmware's flash image."""
import argparse
import hashlib
from pathlib import Path


def stamp(library, output):
    digest = hashlib.sha256(library.read_bytes()).hexdigest()
    text = f'const char airpods_codec_fingerprint[] = "APDG_FDK_SHA256:{digest}";\n'
    if not output.is_file() or output.read_text() != text:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    stamp(args.library, args.output)
