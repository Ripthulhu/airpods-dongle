import contextlib
import io
import pathlib
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import airpods
import audio_diagnostics as transport


class Dongle:
    """Report-3 simulator with independently tracked device values and revisions."""
    def __init__(self):
        self.status = bytearray(64)
        self.status[:7] = b'\x02APDG\x01\x40'
        self.status[7] = 3
        struct.pack_into('<I', self.status, 8, 42)
        self.status[42] = 16
        self.report = bytearray(self.status)
        self.report[0] = 3
        self.values = {0x0d: (10, 7, 2), 0x0a: (5, 9, 1)}
        self.requests = []
        self.setting_writes = []
        self.wire_writes = []
        self.confirm = True
        self.reject = None
        self.disconnect_on_write = False
        self.disconnect_on_upload = False
        self.closed = False
        self.revision = None

    def get(self, report):
        return (self.status if report == 2 else self.report)[:]

    def set(self, data):
        assert len(data) == 64 and data[:7] == b'\x03APDG\x01\x40'
        assert data[21] == 0 and data[20] <= 42
        self.wire_writes.append(bytes(data))
        self.report = bytearray(data)
        op = data[7]
        if op == 7:
            assert data[18:20] == b'\0\0'
            assert struct.unpack_from('<H', data, 16)[0] == data[20]
            self.payload = bytes(data[22:22 + data[20]])
            self.requests.append(self.payload)
            self.report[21] = 1
            struct.pack_into('<H', self.report, 18, data[20])
            if self.disconnect_on_upload:
                struct.pack_into('<I', self.status, 8, 43)
            return
        assert op in (4, 6)
        if op == 6:
            assert not any(data[16:])
            if self.payload == b'\x02':
                self.response = bytes((1, len(self.values))) + b''.join(
                    bytes((control,)) + struct.pack('<III', *value)
                    for control, value in self.values.items())
            else:
                assert len(self.payload) == 6 and self.payload[0] == 1
                if self.reject:
                    self.report[21] = 4
                    self.report[20] = 1
                    self.report[22] = self.reject
                    return
                control = self.payload[1]
                value = struct.unpack_from('<I', self.payload, 2)[0]
                self.setting_writes.append((control, value))
                if self.confirm:
                    previous = self.values.get(control, (0, 0, 0))[1]
                    revision = self.revision if self.revision is not None else ((previous + 1) & 0xffffffff or 1)
                    self.values[control] = (0, revision, value)
                if self.disconnect_on_write:
                    struct.pack_into('<I', self.status, 8, 43)
                self.response = b''
        offset = struct.unpack_from('<H', data, 18)[0]
        body = self.response[offset:offset + 42]
        self.report[7] = 7
        self.report[21] = 3
        struct.pack_into('<HHB', self.report, 16, len(self.response), offset, len(body))
        self.report[22:] = bytes(42)
        self.report[22:22 + len(body)] = body

    def close(self):
        self.closed = True


class Clock:
    now = 0

    def sleep(self, delay):
        self.now += delay


class CliTests(unittest.TestCase):
    def test_apple_encodings_and_percent_bounds(self):
        for name, word, value in [
            ('ear-detection', 'on', 2), ('ear-detection', 'off', 1),
            ('conversation-awareness', 'on', 1), ('mode', 'transparency', 3),
            ('microphone', 'left', 2), ('tone-volume', '75', 0x504b),
            ('case-volume', '0', 0), ('adaptive-strength', '100', 100),
            ('call-controls', 'default', 0x0300), ('crown-direction', 'reversed', 1),
            ('hold-modes', 'anc,transparency,adaptive', 14),
            ('hold-actions', 'siri,noise-control', 0x0105),
        ]:
            self.assertEqual(airpods.encode(name, word), value)
            self.assertEqual(airpods.decode(name, value), word)
        for value in ('-1', '101', '1.5', 'loud', '', '１'):
            with self.assertRaises(ValueError):
                airpods.encode('tone-volume', value)
        for value in ('anc', 'anc,anc', 'anc,loud', 'anc,'):
            with self.assertRaises(ValueError):
                airpods.encode('hold-modes', value)
        for value in ('siri', 'siri,siri,siri', 'siri,unknown'):
            with self.assertRaises(ValueError):
                airpods.encode('hold-actions', value)
        self.assertIn('unknown', airpods.decode('ear-detection', 0))
        self.assertIn('unknown', airpods.decode('tone-volume', 75))

    def test_help_never_opens_hardware(self):
        with patch.object(airpods, 'Hid') as hid, contextlib.redirect_stdout(io.StringIO()) as output:
            for args in (['help'], ['help', 'set'], ['help', 'mode'], ['help', 'hold-modes']):
                self.assertEqual(airpods.main(args), 0)
            for args in (['--help'], ['set', '--help']):
                with self.assertRaises(SystemExit) as result:
                    airpods.main(args)
                self.assertEqual(result.exception.code, 0)
            hid.assert_not_called()
        self.assertIn('transparency', output.getvalue())
        self.assertIn('Examples:', output.getvalue())

    def test_invalid_arguments_never_open_hardware(self):
        with patch.object(airpods, 'Hid') as hid, contextlib.redirect_stderr(io.StringIO()):
            for args in (['set', 'mode', 'loud'], ['set', 'case-volume', '101'], ['help', 'unknown']):
                with self.assertRaises(SystemExit) as result:
                    airpods.main(args)
                self.assertEqual(result.exception.code, 2)
            hid.assert_not_called()

    def test_read_paginated_settings_sends_no_setting_writes(self):
        device = Dongle()
        device.values = {i: (i, i + 1, 1) for i in range(23)}
        values = airpods.SettingsClient(device).read()
        self.assertEqual(len(values), 23)
        self.assertEqual(values[22], (22, 23, 1))
        self.assertEqual(device.requests, [b'\x02'])
        self.assertFalse(device.setting_writes)
        self.assertGreater(sum(p[7] == 4 for p in device.wire_writes), 0)

    def test_confirmed_write_and_other_settings_preserved(self):
        device = Dongle()
        result = airpods.SettingsClient(device).set('mode', 3)
        self.assertIn('confirmed by AirPods', result)
        self.assertEqual(device.setting_writes, [(0x0d, 3)])
        self.assertEqual(device.values[0x0a], (5, 9, 1))
        self.assertEqual(device.requests, [b'\x02', b'\x01\x0d\x03\0\0\0', b'\x02'])

    def test_noop_reports_age_without_a_write(self):
        device = Dongle()
        result = airpods.SettingsClient(device).set('mode', 2)
        self.assertIn('10 ms old; no write sent', result)
        self.assertFalse(device.setting_writes)

    def test_new_value_with_old_revision_does_not_confirm(self):
        device = Dongle()
        device.revision = 7
        clock = Clock()
        with patch.object(airpods.time, 'monotonic', side_effect=lambda: clock.now), \
             patch.object(airpods.time, 'sleep', side_effect=clock.sleep):
            with self.assertRaisesRegex(airpods.Unconfirmed, 'no fresh matching'):
                airpods.SettingsClient(device).set('mode', 3)
        self.assertEqual(device.setting_writes, [(0x0d, 3)])

    def test_revision_wrap_still_confirms(self):
        device = Dongle()
        device.values[0x0d] = (20, 0xffffffff, 2)
        self.assertIn('confirmed', airpods.SettingsClient(device).set('mode', 3))
        self.assertEqual(device.values[0x0d][1], 1)

    def test_rejected_commands_do_not_claim_uncertain_writes(self):
        for code, error in [(7, airpods.Unsupported), (2, airpods.RequestRejected)]:
            device = Dongle()
            device.reject = code
            with self.assertRaises(error):
                airpods.SettingsClient(device).set('mode', 3)
            self.assertFalse(device.setting_writes)

    def test_generation_change_stops_commit_or_confirmation(self):
        device = Dongle()
        device.disconnect_on_upload = True
        with self.assertRaisesRegex(RuntimeError, 'session changed'):
            airpods.SettingsClient(device).read()
        self.assertEqual([p[7] for p in device.wire_writes], [7])
        device = Dongle()
        device.disconnect_on_write = True
        with self.assertRaises(airpods.Unconfirmed):
            airpods.SettingsClient(device).set('mode', 3)
        self.assertEqual(device.setting_writes, [(0x0d, 3)])
        self.assertFalse(any(p[7] == 5 for p in device.wire_writes))

    def test_busy_and_unsupported_session_fail_before_writes(self):
        device = Dongle()
        device.report[21] = 2
        with self.assertRaisesRegex(RuntimeError, 'busy'):
            airpods.SettingsClient(device).read()
        self.assertFalse(device.wire_writes)
        device.status[42] = 0
        with self.assertRaisesRegex(RuntimeError, 'typed settings'):
            airpods.SettingsClient(device).read()

    def test_snapshot_rejects_truncation_duplicates_and_zero_revision(self):
        valid = b'\x01\x01\x0d' + struct.pack('<III', 0, 1, 3)
        self.assertEqual(airpods.snapshot(valid)[0x0d], (0, 1, 3))
        for invalid in (b'', b'\x02\0', valid[:-1], valid + b'\0',
                        b'\x01\x02' + valid[2:] * 2,
                        b'\x01\x01\x0d' + struct.pack('<III', 0, 0, 3)):
            with self.assertRaises(RuntimeError):
                airpods.snapshot(invalid)

    def test_exit_codes_and_handle_cleanup(self):
        device = Dongle()
        with patch.object(airpods, 'Hid', return_value=device) as hid, \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(airpods.main(['--serial', 'PC', 'get', 'mode']), 0)
            hid.assert_called_once_with('PC')
        self.assertIn('anc', output.getvalue())
        self.assertTrue(device.closed)
        device = Dongle()
        with patch.object(airpods, 'Hid', return_value=device), \
             patch.object(airpods.SettingsClient, 'set', side_effect=airpods.Unconfirmed('no report')), \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(airpods.main(['set', 'mode', 'transparency']), 3)
        self.assertTrue(device.closed)

    def test_diagnostic_allowlist_still_blocks_setting_writes(self):
        device = Dongle()
        with self.assertRaises(ValueError):
            transport._transaction(device, b'\x01\x0d\x03\0\0\0')
        self.assertFalse(device.wire_writes)

    def test_rpc_timeout_does_not_retry_a_setting_write(self):
        class SlowReply(Dongle):
            def set(self, data):
                super().set(data)
                if data[7] == 6 and self.payload[0] == 1:
                    self.report[21] = 2
        device = SlowReply()
        clock = Clock()
        with patch.object(airpods.time, 'monotonic', side_effect=lambda: clock.now), \
             patch.object(airpods.time, 'sleep', side_effect=clock.sleep):
            with self.assertRaises(airpods.Unconfirmed):
                airpods.SettingsClient(device).set('mode', 3)
        self.assertEqual(device.setting_writes, [(0x0d, 3)])

    def test_oversized_response_page_is_rejected(self):
        class BadReply(Dongle):
            def set(self, data):
                super().set(data)
                if data[7] == 6:
                    self.report[20] = 43
        with self.assertRaisesRegex(RuntimeError, 'Oversized HID page'):
            airpods.SettingsClient(BadReply()).read()

    def test_microphone_and_unready_control_channel_block_writes(self):
        for flags in (1, 3 | 16):
            device = Dongle()
            device.status[7] = flags
            with self.assertRaises(RuntimeError):
                airpods.SettingsClient(device).set('mode', 3)
            self.assertFalse(device.wire_writes)

    def test_missing_value_exit_code_and_cleanup(self):
        device = Dongle()
        with patch.object(airpods, 'Hid', return_value=device), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(airpods.main(['get', 'case-volume']), 1)
        self.assertIn('unknown', output.getvalue())
        self.assertTrue(device.closed)


if __name__ == '__main__':
    unittest.main()
