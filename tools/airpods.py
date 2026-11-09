#!/usr/bin/env python3
"""Control AirPods through the dongle's existing USB HID interface (Python 3.10+)."""
import argparse
import secrets
import struct
import sys
import time

from audio_diagnostics import Hid, RequestRejected, Unsupported, typed_request
from airpods_att import AttSettings, Unconfirmed as AttUnconfirmed
from airpods_hearing import HearingSettings, PARAMETERS
import airpods_hearing


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
    'en352-protection': (0x37, TOGGLE),
    'gain-swipe': (0x2f, TOGGLE),
    'smart-routing': (0x20, TOGGLE),
    'allow-auto-connect': (0x36, TOGGLE),
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
EXTRA_SETTINGS = {'loud-sound-reduction': TOGGLE, 'game-mode': TOGGLE}
SETTING_NAMES = tuple(SETTINGS) + tuple(EXTRA_SETTINGS) + tuple(PARAMETERS)


def value_help(name):
    if name in PARAMETERS:
        return airpods_hearing.value_help(name)
    kind = EXTRA_SETTINGS[name] if name in EXTRA_SETTINGS else SETTINGS[name][1]
    if isinstance(kind, dict):
        return ', '.join(kind)
    if kind == 'percent':
        return 'integer 0..100'
    if kind == 'modes':
        return 'at least two distinct modes, comma-separated: ' + ', '.join(MODES)
    return 'left,right actions, comma-separated: noise-control or siri for each ear'


def encode(name, value):
    if name in PARAMETERS:
        return airpods_hearing.encode(name, value)
    if name in EXTRA_SETTINGS:
        if value not in EXTRA_SETTINGS[name]:
            raise ValueError(value_help(name))
        return value == 'on'
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


def routing(data):
    if (len(data) != 32 or data[:2] != b'\x01\x20' or data[19] != 0 or
            not struct.unpack_from('<I', data, 4)[0] or data[18] not in (0, 1, 2)):
        raise RuntimeError('Malformed routing snapshot')
    return data


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

    def game(self, enabled=None):
        def read(data):
            if (len(data) != 20 or data[0] != 1 or data[12] not in (0, 1) or
                    data[13] > 6 or data[14] > 2 or data[15] & ~15):
                raise RuntimeError('Malformed audio profile status')
            return data
        current = read(typed_request(self.device, b'\x09', self.generation))
        if enabled is None:
            label = ('off', 'waiting for playback', 'unconfirmed', 'on', 'failed', 'unsupported', 'turning off')[current[13]]
            return f'game-mode: {label} (requested {"on" if current[12] else "off"})'
        if not self.status[7] & 2 or self.status[7] & 16 or current[15] & 4:
            raise RuntimeError('Settings must be ready and microphone use stopped')
        if enabled and not current[15] & 8:
            raise Unsupported('Game Mode is unavailable on this connection')
        if current[12] == enabled and current[13] == (3 if enabled else 0):
            return f'game-mode: {"on" if enabled else "off"} (already confirmed; no write sent)'
        attempted = False
        try:
            attempted = True
            current = read(typed_request(self.device, bytes((4, enabled)), self.generation))
            until = time.monotonic() + 8
            while True:
                if current[12] != enabled:
                    raise RuntimeError('Game Mode choice was superseded')
                if current[13] == (3 if enabled else 0):
                    return f'game-mode: {"on" if enabled else "off"} (confirmed)'
                if enabled and current[13] == 1 and not current[15] & 2:
                    return 'game-mode: on requested; waiting for playback (not yet active)'
                if current[13] in (4, 5) or time.monotonic() >= until:
                    raise RuntimeError('Game Mode did not confirm')
                time.sleep(.1)
                current = read(typed_request(self.device, b'\x09', self.generation))
        except RequestRejected as error:
            if error.code in (1, 2, 4):
                raise
            raise Unconfirmed(f'Game Mode not confirmed: {error}. Read get game-mode before retrying.') from error
        except Unsupported:
            raise
        except (OSError, RuntimeError) as error:
            if attempted:
                raise Unconfirmed(f'Game Mode not confirmed: {error}. Read get game-mode before retrying.') from error
            raise

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

    def basic(self, opcode, replace=False):
        before = self.device.get(2)
        capability = 4 if opcode == 4 else 1
        if not struct.unpack_from('<H', before, 42)[0] & capability:
            raise Unsupported('Firmware does not support this connection command')
        if opcode == 2 and any(before[12:18]) and before[12:18] != b'\xff' * 6 and not replace:
            raise RuntimeError('AirPods are already saved. Use connect, or pair airpods --replace to replace their bond.')
        if opcode in (2, 4) and before[7] & (4 | 8 | 16 | 64):
            raise RuntimeError('Pause playback, stop microphone use and disconnect the phone relay before pairing; no changes sent')
        if opcode == 1 and (not any(before[12:18]) or before[12:18] == b'\xff' * 6):
            raise RuntimeError('No saved AirPods; use pair airpods first')
        sequence = (struct.unpack_from('<I', before, 36)[0] + 1) & 0xffffffff or 1
        request = bytearray(64)
        request[:7] = before[:7]
        request[7] = opcode
        request[8:18] = before[8:18]
        struct.pack_into('<I', request, 18, sequence)
        try:
            self.device.set(request)
            until = time.monotonic() + 4
            while time.monotonic() < until:
                current = self.device.get(2)
                received = struct.unpack_from('<I', current, 36)[0]
                if received == sequence and current[41] == opcode:
                    result = current[40]
                    if result == 1:
                        return
                    if result == 4:
                        raise Unsupported('Firmware rejected this connection command as unsupported')
                    if opcode == 2 and result == 2:
                        raise Unconfirmed('AirPods pairing setup was busy or failed after changing state. Inspect status before retrying.')
                    if result in (2, 3, 5, 6):
                        raise RequestRejected({2: 2, 3: 3, 5: 4, 6: 1}[result])
                    break
                if current[8:18] != before[8:18] or received != struct.unpack_from('<I', before, 36)[0]:
                    break
                time.sleep(.01)
        except (Unsupported, RequestRejected, Unconfirmed):
            raise
        except (OSError, RuntimeError) as error:
            raise Unconfirmed(f'Connection request not confirmed: {error}. It may have applied; inspect status before retrying.') from error
        raise Unconfirmed('Connection request sent, but its receipt was lost or timed out. Inspect status before retrying.')

    def pair(self, target, replace=False):
        self.basic(2 if target == 'airpods' else 4, replace)
        if target == 'phone':
            return 'Phone pairing window opened for 120 seconds. Pair AirPods Dongle in the phone Bluetooth settings.'
        return 'AirPods pairing started; not yet paired. Leave both buds in the open case, flashing white.'

    def connection(self, connect):
        status = self.device.get(2)
        if not struct.unpack_from('<H', status, 42)[0] & 512:
            if not connect:
                raise Unsupported('Disconnect requires newer firmware with routing support; no changes sent')
            self.basic(1)
            return 'AirPods connection requested; not yet confirmed. Use status to check the connection.'
        before = routing(typed_request(self.device, b'\x14'))
        peer = bytes(before[12:18])
        if not any(peer) or peer == b'\xff' * 6 or peer != status[12:18]:
            raise RuntimeError('No matching saved AirPods; use pair airpods first')
        complete = (lambda flags: flags & 0x7f == 0x63) if connect else (lambda flags: flags & 0x7e == 8)
        if complete(before[2]):
            return 'AirPods already connected and audio ownership confirmed.' if connect else 'AirPods already disconnected; automatic reconnect is held off.'
        revision = (struct.unpack_from('<I', before, 4)[0] + 1) & 0xffffffff or 1
        nonce = secrets.randbits(32) or 1
        if nonce == struct.unpack_from('<I', before, 28)[0]:
            nonce = (nonce + 1) & 0xffffffff or 1
        action = 2 if connect else 1
        # A release/acquire can change the session during its reply. Recover only its exact receipt, never resend it.
        payload = bytes((21, int(connect))) + before[4:8] + struct.pack('<I', nonce)
        try:
            current = routing(typed_request(self.device, payload))
        except Unsupported:
            raise
        except RequestRejected as error:
            if error.code in (1, 2, 3, 4):
                raise
            current = None
        except (OSError, RuntimeError):
            current = None
        until = time.monotonic() + 12
        try:
            while time.monotonic() < until:
                if current is None:
                    current = routing(typed_request(self.device, b'\x14'))
                if (current[12:18] != peer or struct.unpack_from('<I', current, 4)[0] != revision or
                        current[18] != action or struct.unpack_from('<I', current, 28)[0] != nonce):
                    raise RuntimeError('Matching routing receipt is missing or was replaced')
                if complete(current[2]):
                    return 'AirPods connected; audio ownership confirmed.' if connect else 'AirPods disconnected; automatic reconnect held off until Connect, a button press or reboot.'
                current = None
                time.sleep(.1)
        except (OSError, RuntimeError) as error:
            raise Unconfirmed(f'Connection change not confirmed: {error}. It may have applied; inspect status before retrying.') from error
        raise Unconfirmed('Connection request accepted, but its final state was not confirmed. Inspect status before retrying.')

    def reconnect_policy(self, mode):
        def decode_policy(reply):
            if len(reply) != 2 or reply[0] != 1 or reply[1] not in (0, 1):
                raise RuntimeError('Malformed reconnect-policy response')
            return ('manual', 'auto')[reply[1]]

        if not struct.unpack_from('<H', self.device.get(2), 42)[0] & 16:
            raise Unsupported('Reconnect policy requires newer firmware; no changes sent')
        try:
            current = decode_policy(typed_request(self.device, b'\x17'))
        except Unsupported as error:
            raise Unsupported('Reconnect policy unavailable: firmware may need updating or its saved configuration is unreadable; no changes sent') from error
        if mode is None:
            return f'Reconnect policy: {current}'
        if mode == current:
            return f'Reconnect policy: {current} (no change needed; no write sent)'
        try:
            confirmed = decode_policy(typed_request(self.device, bytes((23, int(mode == 'auto')))))
            if confirmed != mode:
                raise RuntimeError('Reconnect policy did not match the requested value')
        except Unsupported:
            raise
        except RequestRejected as error:
            if error.code in (1, 2, 3, 4):
                raise
            raise Unconfirmed(f'Reconnect policy not confirmed: {error}. Query reconnect-policy before retrying.') from error
        except (OSError, RuntimeError) as error:
            raise Unconfirmed(f'Reconnect policy not confirmed: {error}. Query reconnect-policy before retrying.') from error
        return f'Reconnect policy: {confirmed} (saved and verified)'


def parser():
    result = argparse.ArgumentParser(description=__doc__, epilog='Close NTPods or other HID control clients before using control commands.')
    result.add_argument('--serial', help='select one Pico by its USB serial; required when multiple dongles are attached')
    commands = result.add_subparsers(dest='command', required=True)
    help_command = commands.add_parser('help', help='show commands or help for a setting')
    help_command.add_argument('topic', nargs='?', help='command or setting name')
    commands.add_parser('status', help='show connection, codec and batteries without changing settings')
    commands.add_parser('settings', help='list cached scalar setting values and their report ages')
    pair = commands.add_parser('pair', help='start AirPods pairing or open phone pairing for 120 seconds',
                               description='Pair new AirPods (default), or open the phone pairing window. Pause playback and microphone use first. Use connect for an existing AirPods bond.')
    pair.add_argument('target', nargs='?', default='airpods', choices=('airpods', 'phone'))
    pair.add_argument('--replace', action='store_true', help='AirPods only: erase the saved AirPods bond before pairing its replacement')
    commands.add_parser('connect', help='explicitly reconnect to the saved AirPods and request audio ownership',
                        description='Reconnect using the saved AirPods bond, without pairing again. This explicit request can reclaim audio from another host. New firmware confirms audio ownership; older firmware confirms only that the request was accepted.')
    commands.add_parser('disconnect', help='release AirPods and hold automatic reconnect off until Connect, a button press or reboot',
                        description='Release the AirPods without erasing pairing. Automatic reconnect stays held off until Connect, a button press or reboot, including when the saved policy is Auto. Requires firmware with routing support.')
    policy = commands.add_parser('reconnect-policy', help='read or save manual/automatic reconnect behavior (new firmware)',
                                 description='Read the reconnect policy, or change it while playback, microphone use and connection transitions are idle. Manual is the default: use Connect or one button press. Auto explicitly allows reclaiming AirPods from another host on subsequent eligible attempts. A Disconnect hold overrides Auto until Connect, a button press or reboot. Older firmware rejects policy commands without changes.')
    policy.add_argument('mode', nargs='?', choices=('manual', 'auto'), help='omit to read; auto may interrupt another host')
    get = commands.add_parser('get', help='read one reported setting')
    get.add_argument('setting', choices=SETTING_NAMES)
    set_command = commands.add_parser('set', help='change one setting and wait for device confirmation')
    set_command.add_argument('setting', choices=SETTING_NAMES)
    set_command.add_argument('value', help='named value; use help SETTING to see accepted values')
    return result, commands.choices


def main(argv=None):
    cli, commands = parser()
    args = cli.parse_args(argv)
    if args.command == 'help':
        if args.topic in SETTING_NAMES:
            print(f'{args.topic}: {value_help(args.topic)}')
            print(f'Get: airpods.py get {args.topic}\nSet: airpods.py set {args.topic} VALUE')
        elif args.topic in commands:
            commands[args.topic].print_help()
        elif args.topic is None:
            cli.print_help()
            print('\nSettings:')
            for name in SETTING_NAMES:
                print(f'  {name}: {value_help(name)}')
            print('\nExamples:\n  airpods.py connect\n  airpods.py pair airpods\n  airpods.py pair phone\n  airpods.py disconnect\n  airpods.py reconnect-policy manual\n  airpods.py get mode\n  airpods.py set mode transparency\n  airpods.py set ear-detection on')
        else:
            cli.error('unknown help topic: ' + args.topic)
        return 0
    if args.command == 'pair' and args.target == 'phone' and args.replace:
        cli.error('--replace applies only to pair airpods')
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
        elif args.command in ('get', 'set') and args.setting == 'loud-sound-reduction':
            att = AttSettings(device, client.generation)
            if args.command == 'get':
                print('loud-sound-reduction: ' + ('on' if att.get_loud_sound_reduction() else 'off'))
            else:
                changed = att.set_loud_sound_reduction(value)
                print(f'loud-sound-reduction: {args.value} (' + ('confirmed by AirPods)' if changed else 'already read; no write sent)'))
        elif args.command in ('get', 'set') and args.setting == 'game-mode':
            print(client.game(value if args.command == 'set' else None))
        elif args.command in ('get', 'set') and args.setting in PARAMETERS:
            hearing = HearingSettings(device, client.generation)
            if args.command == 'get':
                result = hearing.get(args.setting)
                rendered = ('on' if result else 'off') if type(result) is bool else str(result)
                print(f'{args.setting}: {rendered}')
            else:
                changed = hearing.set(args.setting, value)
                print(f'{args.setting}: {args.value} (' + ('confirmed by AirPods)' if changed else 'already read; no write sent)'))
        elif args.command == 'set':
            print(client.set(args.setting, value))
        elif args.command == 'pair':
            print(client.pair(args.target, args.replace))
        elif args.command in ('connect', 'disconnect'):
            print(client.connection(args.command == 'connect'))
        elif args.command == 'reconnect-policy':
            print(client.reconnect_policy(args.mode))
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
    except (Unconfirmed, AttUnconfirmed) as error:
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
