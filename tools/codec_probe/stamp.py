#!/usr/bin/env python3
"""Retain the exact codec archive hash in the firmware's flash image."""
import argparse
import hashlib
import re
from pathlib import Path


def stamp(library, output, label="FDK"):
    if re.fullmatch(r"[A-Z][A-Z0-9_]*", label) is None:
        raise ValueError("Fingerprint label must contain uppercase letters, digits or underscores")
    digest = hashlib.sha256(library.read_bytes()).hexdigest()
    symbol = "airpods_codec_fingerprint" if label == "FDK" else f"airpods_{label.lower()}_fingerprint"
    text = f'const char {symbol}[] = "APDG_{label}_SHA256:{digest}";\n'
    if not output.is_file() or output.read_text() != text:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--label", default="FDK")
    args = parser.parse_args()
    stamp(args.library, args.output, args.label)
