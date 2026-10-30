#!/usr/bin/env python3
"""Control AirPods through the dongle's existing USB HID interface (Python 3.10+)."""
import argparse
import struct
import sys
import time

from audio_diagnostics import Hid, RequestRejected, Unsupported, typed_request


MODES = {'off': 1, 'anc': 2, 'transparency': 3, 'adaptive': 4}
TOGGLE = {'on': 1, 'off': 2}
HOLD = {'noise-control': 1, 'siri': 5}
# Values match the app and firmware catalog; model gating stays in firmware.
SETTINGS = {
    'mode': (0x0d, MODES),
    'microphone': (0x01, {'auto': 0, 'right': 1, 'left': 2}),
    'ear-detection': (0x0a, {'on': 2, 'off': 1}),
    'one-bud-anc': (0x1b, TOGGLE),
    'conversation-awareness': (0x28, TOGGLE),
    'personalized-volume': (0x26, TOGGLE),
    'allow-off': (0x34, TOGGLE),
    'pause-when-asleep': (0x35, TOGGLE),
    'case-sounds': (0x31, TOGGLE),
    'optimized-charging': (0x3b, TOGGLE),
    'volume-swipe': (0x25, TOGGLE),
    'press-speed': (0x17, {'default': 0, 'slower': 1, 'slowest': 2}),
    'hold-duration': (0x18, {'default': 0, 'slower': 1, 'slowest': 2}),
    'swipe-duration': (0x23, {'default': 0, 'longer': 1, 'longest': 2}),
    'call-controls': (0x24, {'default': 0x0300, 'swapped': 0x0200}),
    'crown-direction': (0x1c, {'default': 2, 'reversed': 1}),
    'tone-volume': (0x1f, 'percent'),
    'case-volume': (0x40, 'percent'),
    'adaptive-strength': (0x2e, 'percent'),
    'hold-modes': (0x1a, 'modes'),
    'hold-actions': (0x16, 'actions'),
}


def value_help(name):
    kind = SETTINGS[name][1]
    if isinstance(kind, dict):
        return ', '.join(kind)
    if kind == 'percent':
        return 'integer 0..100'
    if kind == 'modes':
        return 'at least two distinct modes, comma-separated: ' + ', '.join(MODES)
    return 'left,right actions, comma-separated: noise-control or siri for each ear'


def encode(name, value):
    control, kind = SETTINGS[name]
    if isinstance(kind, dict):
        if value not in kind:
            raise ValueError(value_help(name))
        return kind[value]
    if kind == 'percent':
        if not value.isascii() or not value.isdecimal() or not 0 <= int(value) <= 100:
            raise ValueError(value_help(name))
        return int(value) | (0x5000 if control == 0x1f else 0)
    parts = value.split(',')
    if kind == 'modes':
        if len(parts) < 2 or len(set(parts)) != len(parts) or any(p not in MODES for p in parts):
            raise ValueError(value_help(name))
        return sum(1 << (MODES[p] - 1) for p in parts)
    if len(parts) != 2 or any(p not in HOLD for p in parts):
        raise ValueError(value_help(name))
    return HOLD[parts[0]] | HOLD[parts[1]] << 8


def decode(name, raw):
    control, kind = SETTINGS[name]
    if isinstance(kind, dict):
        for word, value in kind.items():
            if raw == value:
                return word
    elif kind == 'percent':
        value = raw & 0xff if control == 0x1f else raw
        if raw == (value | (0x5000 if control == 0x1f else 0)) and value <= 100:
            return str(value)
    elif kind == 'modes' and raw <= 15 and raw.bit_count() >= 2:
        return ','.join(word for word, bit in MODES.items() if raw & (1 << (bit - 1)))
    elif kind == 'actions':
        actions = {v: k for k, v in HOLD.items()}
        if raw <= 0xffff and (raw & 255) in actions and (raw >> 8) in actions:
            return actions[raw & 255] + ',' + actions[raw >> 8]
    return f'unknown device value (0x{raw:x})'


def snapshot(data):
    if len(data) < 2 or data[0] != 1 or len(data) != 2 + 13 * data[1]:
        raise RuntimeError('Malformed settings snapshot')
    result = {}
    for offset in range(2, len(data), 13):
        control = data[offset]
        age, revision, value = struct.unpack_from('<III', data, offset + 1)
        if control in result or revision == 0:
            raise RuntimeError('Malformed settings record')
        result[control] = (age, revision, value)
    return result


class Unconfirmed(RuntimeError):
    pass


class SettingsClient:
    def __init__(self, device):
        self.device = device
        self.status = device.get(2)
        self.generation = struct.unpack_from('<I', self.status, 8)[0]

    def read(self):
        if not struct.unpack_from('<H', self.status, 42)[0] & 16:
            raise RuntimeError('Firmware does not advertise typed settings support')
        return snapshot(typed_request(self.device, b'\x02', self.generation))

    def set(self, name, value):
        control = SETTINGS[name][0]
        if not self.status[7] & 2:
            raise RuntimeError('AirPods settings connection is not ready')
        if self.status[7] & 16:
            raise RuntimeError('Stop microphone use before changing AirPods settings')
        before = self.read().get(control)
        if before and before[2] == value:
            return f'{name}: {decode(name, value)} (already reported, {before[0]} ms old; no write sent)'
        try:
            reply = typed_request(self.device, bytes((1, control)) + struct.pack('<I', value), self.generation)
            if reply:
                raise RuntimeError('Unexpected setting-write response')
        except Unsupported:
            raise
        except RequestRejected as error:
            if error.code in (1, 2, 4):
                raise
            raise Unconfirmed(f'{name}: change not confirmed: {error}. Read it again before retrying.') from error
        except (OSError, RuntimeError) as error:
            raise Unconfirmed(f'{name}: change not confirmed: {error}. It may have applied; read it again before retrying.') from error
        try:
            until = time.monotonic() + 5
            while time.monotonic() < until:
                current = self.read().get(control)
                if current and (before is None or current[1] != before[1]) and current[2] == value:
                    return f'{name}: {decode(name, value)} (confirmed by AirPods)'
                time.sleep(.1)
        except (OSError, RuntimeError) as error:
            raise Unconfirmed(f'{name}: change not confirmed: {error}. It may have applied; read it again before retrying.') from error
        raise Unconfirmed(f'{name}: sent, but no fresh matching device report. It may have applied; use get {name} before retrying.')


def parser():
    result = argparse.ArgumentParser(description=__doc__, epilog='Close NTPods or other HID control clients before using settings commands.')
    result.add_argument('--serial', help='select one Pico by its USB serial; required when multiple dongles are attached')
    commands = result.add_subparsers(dest='command', required=True)
    help_command = commands.add_parser('help', help='show commands or help for a setting')
    help_command.add_argument('topic', nargs='?', help='command or setting name')
    commands.add_parser('status', help='show connection, codec and batteries without changing settings')
    commands.add_parser('settings', help='list readable setting values and their report ages')
    get = commands.add_parser('get', help='read one reported setting')
    get.add_argument('setting', choices=SETTINGS)
    set_command = commands.add_parser('set', help='change one setting and wait for device confirmation')
    set_command.add_argument('setting', choices=SETTINGS)
    set_command.add_argument('value', help='named value; use help SETTING to see accepted values')
    return result, commands.choices


def main(argv=None):
    cli, commands = parser()
    args = cli.parse_args(argv)
    if args.command == 'help':
        if args.topic in SETTINGS:
            print(f'{args.topic}: {value_help(args.topic)}')
            print(f'Get: airpods.py get {args.topic}\nSet: airpods.py set {args.topic} VALUE')
        elif args.topic in commands:
            commands[args.topic].print_help()
        elif args.topic is None:
            cli.print_help()
            print('\nSettings:')
            for name in SETTINGS:
                print(f'  {name}: {value_help(name)}')
            print('\nExamples:\n  airpods.py get mode\n  airpods.py set mode transparency\n  airpods.py set ear-detection on')
        else:
            cli.error('unknown help topic: ' + args.topic)
        return 0
    if args.command == 'set':
        try:
            value = encode(args.setting, args.value)
        except ValueError as error:
            cli.error(f'{args.setting}: expected {error}')
    device = None
    try:
        device = Hid(args.serial)
        client = SettingsClient(device)
        if args.command == 'status':
            data = client.status
            print('AirPods: ' + ('connected' if data[7] & 1 else 'disconnected'))
            print('Settings: ' + ('ready' if data[7] & 2 else 'not ready'))
            print('Codec: ' + {0: 'unknown', 1: 'SBC', 2: 'AAC', 3: 'AAC-ELD'}.get(data[23], 'unknown'))
            for label, raw in zip(('left', 'right', 'case'), data[19:22]):
                print(f'Battery {label}: ' + (f'{raw}%' if raw <= 100 else 'unknown'))
        elif args.command == 'set':
            print(client.set(args.setting, value))
        else:
            values = client.read()
            for name in ([args.setting] if args.command == 'get' else SETTINGS):
                record = values.get(SETTINGS[name][0])
                if record is None:
                    print(f'{name}: unknown (not reported; may be unsupported)')
                    if args.command == 'get':
                        return 1
                else:
                    print(f'{name}: {decode(name, record[2])} (report age {record[0]} ms)')
        return 0
    except Unconfirmed as error:
        print(str(error), file=sys.stderr)
        return 3
    except (OSError, RuntimeError) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1
    finally:
        if device is not None:
            device.close()


if __name__ == '__main__':
    sys.exit(main())
