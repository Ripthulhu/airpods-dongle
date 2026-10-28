#!/usr/bin/env python3
"""Bounded Game Mode or BLE-scanning A/B capture, restoring the original mode.

Requires firmware feature 11/12. No codec, volume or hearing settings are changed.
The normal audio_diagnostics.py collector remains read-only.
"""
import argparse
import json
import sys
import time
from audio_diagnostics import Hid, _transaction, sample


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--game', choices=('on', 'off'))
    mode.add_argument('--scan', choices=('on', 'off'))
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--serial')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 90:
        parser.error('--seconds must be 1..90')
    output = open(args.output, 'x', encoding='utf-8')
    device = Hid(args.serial)
    changed = False

    def emit(value):
        print(json.dumps(value), file=output, flush=True)

    try:
        original = sample(device)
        if not original.get('trace') or original['audio_flags'] & 4:
            raise RuntimeError('Requires trace-capable firmware and an idle microphone')
        if args.scan and not original['trace']['stats']['ble_scan_requested']:
            raise RuntimeError('Scanning must initially be running for this comparison')
        emit(dict(event='before', value=original))
        _transaction(device, bytes((4 if args.game else 12,
                                   int((args.game or args.scan) == 'on'))))
        changed = True
        emit(dict(event='profile', game=args.game, scan=args.scan, time=time.time()))
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            start = time.monotonic()
            emit(sample(device))
            time.sleep(max(0, 1 - (time.monotonic() - start)))
    finally:
        try:
            if changed:
                _transaction(device, bytes((4, int(original['game_requested'])) if args.game else (12, 1)))
                emit(dict(event='restored', value=sample(device)))
        finally:
            device.close()
            output.close()


if __name__ == '__main__':
    main()
