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


class NewSettingsTest(unittest.TestCase):
    def test_scalar_gaps_have_exact_wire_values_and_help(self):
        for name, control in (('en352-protection', 0x37), ('gain-swipe', 0x2f),
                              ('smart-routing', 0x20), ('allow-auto-connect', 0x36)):
            self.assertEqual(airpods.SETTINGS[name][0], control)
            self.assertEqual(airpods.encode(name, 'on'), 1)
            self.assertEqual(airpods.encode(name, 'off'), 2)
            with patch.object(airpods, 'Hid') as hid, contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(airpods.main(['help', name]), 0)
                self.assertIn('on, off', output.getvalue())
                hid.assert_not_called()

    def test_complex_values_validate_before_opening_hardware(self):
        with patch.object(airpods, 'Hid') as hid, contextlib.redirect_stderr(io.StringIO()):
            for name, value in (('loud-sound-reduction', 'maybe'), ('eq-low', '2.5'),
                                ('hearing-amplification', 'nan'), ('transparency-tone', 'inf'),
                                ('accommodations-preset', 'fake-audiogram')):
                with self.assertRaises(SystemExit) as error:
                    airpods.main(['set', name, value])
                self.assertEqual(error.exception.code, 2)
            hid.assert_not_called()

    def test_complex_confirmation_and_uncertainty_exit_codes(self):
        for name, cls, method in (('loud-sound-reduction', airpods.AttSettings, 'set_loud_sound_reduction'),
                                  ('hearing-aid', airpods.HearingSettings, 'set')):
            for outcome, code in ((True, 0), (airpods.AttUnconfirmed('read again'), 3)):
                device = Dongle()
                with patch.object(airpods, 'Hid', return_value=device), patch.object(cls, '__init__', return_value=None), \
                     patch.object(cls, method, side_effect=outcome if isinstance(outcome, Exception) else None,
                                  return_value=outcome), contextlib.redirect_stdout(io.StringIO()), \
                     contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(airpods.main(['set', name, 'on']), code)
                self.assertTrue(device.closed)

    def test_game_mode_confirms_or_reports_waiting_without_replaying(self):
        device = Dongle()
        baseline = bytearray(20)
        baseline[0] = 1
        baseline[15] = 8 | 2
        active = bytearray(baseline)
        active[12:14] = bytes((1, 3))
        with patch.object(airpods, 'typed_request', side_effect=(baseline, active)) as request:
            self.assertIn('on (confirmed)', airpods.SettingsClient(device).game(True))
            self.assertEqual(request.call_args_list[-1].args[1], b'\x04\x01')
        pending = bytearray(active)
        pending[13], pending[15] = 1, 8
        with patch.object(airpods, 'typed_request', side_effect=(baseline, pending)) as request:
            self.assertIn('waiting for playback (not yet active)', airpods.SettingsClient(device).game(True))
            self.assertEqual(request.call_count, 2)

    def test_game_mode_known_rejection_differs_from_uncertain_confirmation(self):
        baseline = bytearray(20)
        baseline[0], baseline[15] = 1, 8 | 2
        for error, expected in ((airpods.RequestRejected(2), airpods.RequestRejected),
                                (OSError('lost'), airpods.Unconfirmed)):
            with patch.object(airpods, 'typed_request', side_effect=(baseline, error)):
                with self.assertRaises(expected):
                    airpods.SettingsClient(Dongle()).game(True)


class ConnectionDongle(Dongle):
    def __init__(self):
        super().__init__()
        self.status[12:18] = bytes.fromhex('010203040506')
        struct.pack_into('<H', self.status, 42, 1 | 4 | 16 | 512)
        self.route = bytearray(32)
        self.route[:2] = b'\x01\x20'
        struct.pack_into('<I', self.route, 4, 10)
        self.route[12:18] = self.status[12:18]
        self.basic_result = 1
        self.basic_receipt = True
        self.basic_generation_change = False
        self.basic_writes = []
        self.routing_writes = []
        self.routing_generation_change = False
        self.routing_reads = 0
        self.complete_after = 2
        self.supersede = None
        self.policy = 0
        self.policy_writes = []
        self.policy_reply = None
        self.fail_policy_reply = False

    def set(self, data):
        if data[0] == 2:
            self.wire_writes.append(bytes(data))
            self.basic_writes.append(bytes(data))
            if self.basic_receipt:
                self.status[36:40] = data[18:22]
                self.status[40:42] = bytes((self.basic_result, data[7]))
            if self.basic_generation_change:
                struct.pack_into('<I', self.status, 8, 43)
                self.status[12:18] = bytes(6)
            return
        if data[7] != 6 or self.payload[0] not in (20, 21, 23):
            return super().set(data)
        self.wire_writes.append(bytes(data))
        self.report = bytearray(data)
        feature = self.payload[0]
        if self.reject:
            self.report[21] = 4
            self.report[20] = 1
            self.report[22] = self.reject
            return
        if feature == 20:
            if self.routing_writes:
                self.routing_reads += 1
                if self.complete_after is not None and self.routing_reads >= self.complete_after:
                    self.route[2] = 0x63 if self.route[18] == 2 else 8
                if self.supersede:
                    self.supersede(self.route)
            self.response = bytes(self.route)
        elif feature == 21:
            self.routing_writes.append(self.payload)
            assert len(self.payload) == 10 and self.payload[1] in (0, 1)
            assert self.payload[2:6] == self.route[4:8]
            revision = (struct.unpack_from('<I', self.route, 4)[0] + 1) & 0xffffffff or 1
            struct.pack_into('<I', self.route, 4, revision)
            self.route[18] = self.payload[1] + 1
            self.route[28:32] = self.payload[6:10]
            self.route[2] = 4 if self.payload[1] else 16
            self.response = bytes(self.route)
            if self.routing_generation_change:
                struct.pack_into('<I', self.status, 8, 43)
        else:
            assert len(self.payload) in (1, 2)
            if len(self.payload) == 2:
                self.policy_writes.append(self.payload[1])
                self.policy = self.payload[1]
                if self.fail_policy_reply:
                    raise OSError('reply lost after flash write')
            self.response = self.policy_reply if self.policy_reply is not None else bytes((1, self.policy))
        self.report[7] = 7
        self.report[21] = 3
        struct.pack_into('<HHB', self.report, 16, len(self.response), 0, len(self.response))
        self.report[22:] = bytes(42)
        self.report[22:22 + len(self.response)] = self.response


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
            for args in (['help'], ['help', 'set'], ['help', 'mode'], ['help', 'hold-modes'],
                         ['help', 'pair'], ['help', 'connect'], ['help', 'disconnect'], ['help', 'reconnect-policy']):
                self.assertEqual(airpods.main(args), 0)
            for args in (['--help'], ['set', '--help'], ['pair', '--help'], ['reconnect-policy', '--help']):
                with self.assertRaises(SystemExit) as result:
                    airpods.main(args)
                self.assertEqual(result.exception.code, 0)
            hid.assert_not_called()
        self.assertIn('transparency', output.getvalue())
        self.assertIn('Examples:', output.getvalue())

    def test_invalid_arguments_never_open_hardware(self):
        with patch.object(airpods, 'Hid') as hid, contextlib.redirect_stderr(io.StringIO()):
            for args in (['set', 'mode', 'loud'], ['set', 'case-volume', '101'], ['help', 'unknown'],
                         ['pair', 'speaker'], ['pair', 'phone', '--replace'], ['reconnect-policy', 'sometimes']):
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

    def test_pair_existing_bond_requires_explicit_replacement(self):
        device = ConnectionDongle()
        with self.assertRaisesRegex(RuntimeError, 'Use connect'):
            airpods.SettingsClient(device).pair('airpods')
        self.assertFalse(device.wire_writes)
        result = airpods.SettingsClient(device).pair('airpods', replace=True)
        self.assertIn('started; not yet paired', result)
        self.assertEqual(len(device.basic_writes), 1)
        self.assertEqual(device.basic_writes[0][7], 2)

    def test_pair_new_airpods_and_phone_window_are_distinct(self):
        for target, opcode, text in [('airpods', 2, 'not yet paired'), ('phone', 4, '120 seconds')]:
            for empty_peer in (bytes(6), b'\xff' * 6):
                device = ConnectionDongle()
                device.status[12:18] = empty_peer
                self.assertIn(text, airpods.SettingsClient(device).pair(target))
                self.assertEqual([request[7] for request in device.basic_writes], [opcode])
                self.assertEqual(device.basic_writes[0][12:18], empty_peer)
                self.assertFalse(device.routing_writes)

    def test_basic_command_uses_fresh_target_and_sequence_across_cli_sessions(self):
        device = ConnectionDongle()
        struct.pack_into('<I', device.status, 36, 98)
        client = airpods.SettingsClient(device)
        struct.pack_into('<I', device.status, 8, 44)
        client.pair('phone')
        airpods.SettingsClient(device).pair('phone')
        for request, sequence in zip(device.basic_writes, (99, 100)):
            self.assertEqual(request[:7], b'\x02APDG\x01\x40')
            self.assertEqual(request[8:18], device.status[8:18])
            self.assertEqual(struct.unpack_from('<I', request, 18)[0], sequence)
            self.assertFalse(any(request[22:]))
        struct.pack_into('<I', device.status, 36, 0xffffffff)
        airpods.SettingsClient(device).pair('phone')
        self.assertEqual(struct.unpack_from('<I', device.basic_writes[-1], 18)[0], 1)

    def test_basic_pair_receipt_survives_its_own_session_and_selected_peer_change(self):
        device = ConnectionDongle()
        device.basic_generation_change = True
        self.assertIn('pairing started', airpods.SettingsClient(device).pair('airpods', replace=True))
        self.assertEqual(len(device.basic_writes), 1)

    def test_basic_rejection_codes_are_not_typed_rpc_codes(self):
        for result, code in [(2, 2), (3, 3), (5, 4), (6, 1)]:
            device = ConnectionDongle()
            device.basic_result = result
            with self.assertRaises(airpods.RequestRejected) as error:
                airpods.SettingsClient(device).pair('phone')
            self.assertEqual(error.exception.code, code)
            self.assertEqual(len(device.basic_writes), 1)
        device = ConnectionDongle()
        device.basic_result = 4
        with self.assertRaises(airpods.Unsupported):
            airpods.SettingsClient(device).pair('phone')

    def test_pair_preflight_blocks_active_audio_and_post_submit_failure_is_uncertain(self):
        for flag in (4, 8, 16, 64):
            device = ConnectionDongle()
            device.status[7] |= flag
            with self.assertRaisesRegex(RuntimeError, 'no changes sent'):
                airpods.SettingsClient(device).pair('airpods', replace=True)
            self.assertFalse(device.wire_writes)
        device = ConnectionDongle()
        device.basic_result = 2
        with self.assertRaisesRegex(airpods.Unconfirmed, 'failed after changing state'):
            airpods.SettingsClient(device).pair('airpods', replace=True)
        self.assertEqual(len(device.basic_writes), 1)

    def test_missing_basic_receipt_times_out_without_retry(self):
        device = ConnectionDongle()
        device.basic_receipt = False
        clock = Clock()
        with patch.object(airpods.time, 'monotonic', side_effect=lambda: clock.now), \
             patch.object(airpods.time, 'sleep', side_effect=clock.sleep):
            with self.assertRaisesRegex(airpods.Unconfirmed, 'receipt was lost or timed out'):
                airpods.SettingsClient(device).pair('phone')
        self.assertGreaterEqual(clock.now, 4)
        self.assertEqual(len(device.basic_writes), 1)

    def test_legacy_connect_uses_existing_command_but_disconnect_has_no_destructive_fallback(self):
        device = ConnectionDongle()
        struct.pack_into('<H', device.status, 42, 1 | 4 | 16)
        self.assertIn('not yet confirmed', airpods.SettingsClient(device).connection(True))
        self.assertEqual([request[7] for request in device.basic_writes], [1])
        with self.assertRaisesRegex(airpods.Unsupported, 'Disconnect requires newer firmware'):
            airpods.SettingsClient(device).connection(False)
        self.assertEqual([request[7] for request in device.basic_writes], [1])

    def test_no_saved_peer_blocks_connect_without_starting_pairing(self):
        for capabilities in (1 | 16, 1 | 16 | 512):
            device = ConnectionDongle()
            struct.pack_into('<H', device.status, 42, capabilities)
            device.status[12:18] = device.route[12:18] = bytes(6)
            with self.assertRaisesRegex(RuntimeError, 'pair airpods first'):
                airpods.SettingsClient(device).connection(True)
            self.assertFalse(device.basic_writes)
            self.assertFalse(device.routing_writes)

    def test_routing_waits_for_exact_receipt_and_completed_state(self):
        for connect, action in ((True, 2), (False, 1)):
            device = ConnectionDongle()
            device.complete_after = 3
            with patch.object(airpods.secrets, 'randbits', return_value=1234):
                result = airpods.SettingsClient(device).connection(connect)
            self.assertIn('ownership confirmed' if connect else 'automatic reconnect held off', result)
            self.assertEqual(device.routing_writes, [bytes((21, int(connect))) + struct.pack('<II', 10, 1234)])
            self.assertEqual(device.routing_reads, 3)
            self.assertEqual(device.route[18], action)
            self.assertFalse(device.basic_writes)

    def test_disconnect_sets_hold_when_already_disconnected(self):
        device = ConnectionDongle()
        self.assertEqual(device.status[7] & 1, 1)
        device.status[7] = 0
        self.assertEqual(device.route[2], 0)
        airpods.SettingsClient(device).connection(False)
        self.assertEqual(len(device.routing_writes), 1)
        self.assertEqual(device.route[2], 8)
        self.assertIn('already disconnected', airpods.SettingsClient(device).connection(False))
        self.assertEqual(len(device.routing_writes), 1)

    def test_routing_avoids_noop_and_nonce_collisions_and_handles_revision_wrap(self):
        device = ConnectionDongle()
        device.route[2] = 0x63
        self.assertIn('already connected', airpods.SettingsClient(device).connection(True))
        self.assertFalse(device.routing_writes)
        device.route[2] = 0
        struct.pack_into('<I', device.route, 4, 0xffffffff)
        struct.pack_into('<I', device.route, 28, 1)
        with patch.object(airpods.secrets, 'randbits', return_value=0):
            airpods.SettingsClient(device).connection(True)
        self.assertEqual(struct.unpack_from('<II', device.routing_writes[0], 2), (0xffffffff, 2))
        self.assertEqual(struct.unpack_from('<I', device.route, 4)[0], 1)

    def test_routing_recovers_matching_receipt_after_its_own_generation_change_without_retry(self):
        device = ConnectionDongle()
        device.routing_generation_change = True
        self.assertIn('ownership confirmed', airpods.SettingsClient(device).connection(True))
        self.assertEqual(len(device.routing_writes), 1)
        self.assertEqual(struct.unpack_from('<I', device.status, 8)[0], 43)

    def test_routing_does_not_accept_other_peer_revision_action_or_nonce(self):
        for offset in (12, 4, 18, 28):
            device = ConnectionDongle()
            device.supersede = lambda reply, offset=offset: reply.__setitem__(offset, 1 if offset == 18 else reply[offset] ^ 1)
            with self.assertRaisesRegex(airpods.Unconfirmed, 'receipt is missing or was replaced'):
                airpods.SettingsClient(device).connection(True)
            self.assertEqual(len(device.routing_writes), 1)
            self.assertFalse(device.basic_writes)

    def test_routing_rejection_is_not_retried_or_fallen_back(self):
        device = ConnectionDongle()
        actual = airpods.typed_request

        def reject_change(device, payload, generation=None):
            if payload[0] == 21:
                raise airpods.RequestRejected(2)
            return actual(device, payload, generation)

        with patch.object(airpods, 'typed_request', side_effect=reject_change):
            with self.assertRaises(airpods.RequestRejected):
                airpods.SettingsClient(device).connection(True)
        self.assertFalse(device.routing_writes)
        self.assertFalse(device.basic_writes)

    def test_accepted_routing_does_not_claim_final_state_on_timeout(self):
        device = ConnectionDongle()
        device.complete_after = None
        clock = Clock()
        with patch.object(airpods.time, 'monotonic', side_effect=lambda: clock.now), \
             patch.object(airpods.time, 'sleep', side_effect=clock.sleep):
            with self.assertRaisesRegex(airpods.Unconfirmed, 'accepted, but its final state was not confirmed'):
                airpods.SettingsClient(device).connection(True)
        self.assertEqual(len(device.routing_writes), 1)

    def test_reconnect_policy_query_and_verified_save(self):
        device = ConnectionDongle()
        client = airpods.SettingsClient(device)
        self.assertEqual(client.reconnect_policy(None), 'Reconnect policy: manual')
        self.assertFalse(device.policy_writes)
        self.assertIn('saved and verified', client.reconnect_policy('auto'))
        self.assertEqual(device.policy_writes, [1])
        self.assertIn('no change needed; no write sent', client.reconnect_policy('auto'))
        self.assertEqual(device.policy_writes, [1])
        self.assertFalse(device.routing_writes)
        self.assertFalse(device.basic_writes)

    def test_reconnect_policy_set_rejection_is_not_retried(self):
        device = ConnectionDongle()
        actual = airpods.typed_request
        calls = []

        def reject_write(device, payload, generation=None):
            calls.append(bytes(payload))
            if len(payload) == 2:
                raise airpods.RequestRejected(2)
            return actual(device, payload, generation)

        with patch.object(airpods, 'typed_request', side_effect=reject_write):
            with self.assertRaises(airpods.RequestRejected):
                airpods.SettingsClient(device).reconnect_policy('auto')
        self.assertEqual(calls, [b'\x17', b'\x17\x01'])
        self.assertFalse(device.policy_writes)

    def test_older_firmware_policy_rejected_without_changes(self):
        device = ConnectionDongle()
        device.reject = 7
        with self.assertRaisesRegex(airpods.Unsupported, 'firmware may need updating or its saved configuration is unreadable'):
            airpods.SettingsClient(device).reconnect_policy('auto')
        self.assertFalse(device.policy_writes)
        self.assertFalse(device.routing_writes)

    def test_uncertain_policy_write_is_not_retried(self):
        device = ConnectionDongle()
        device.fail_policy_reply = True
        with self.assertRaisesRegex(airpods.Unconfirmed, 'Query reconnect-policy before retrying'):
            airpods.SettingsClient(device).reconnect_policy('auto')
        self.assertEqual(device.policy, 1)
        self.assertEqual(device.policy_writes, [1])

    def test_policy_write_requires_matching_readback(self):
        class WrongReadback(ConnectionDongle):
            def set(self, data):
                super().set(data)
                if data[7] == 6 and self.payload == b'\x17\x01':
                    self.report[23] = 0

        device = WrongReadback()
        with self.assertRaisesRegex(airpods.Unconfirmed, 'did not match'):
            airpods.SettingsClient(device).reconnect_policy('auto')
        self.assertEqual(device.policy_writes, [1])

    def test_policy_read_rejects_malformed_responses(self):
        for reply in (b'', b'\x01', b'\x02\0', b'\x01\x02', b'\x01\0\0'):
            device = ConnectionDongle()
            device.policy_reply = reply
            with self.assertRaisesRegex(RuntimeError, 'Malformed reconnect-policy'):
                airpods.SettingsClient(device).reconnect_policy(None)
            self.assertFalse(device.policy_writes)

    def test_new_commands_select_serial_and_always_close_handle(self):
        for args in (['pair', 'phone'], ['connect'], ['disconnect'], ['reconnect-policy'], ['reconnect-policy', 'auto']):
            device = ConnectionDongle()
            with patch.object(airpods, 'Hid', return_value=device) as hid, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(airpods.main(['--serial', 'PHONE'] + args), 0)
                hid.assert_called_once_with('PHONE')
            self.assertTrue(device.closed)
        device = ConnectionDongle()
        device.routing_generation_change = True
        device.supersede = lambda reply: reply.__setitem__(12, 99)
        with patch.object(airpods, 'Hid', return_value=device) as hid, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(airpods.main(['--serial', 'PC', 'connect']), 3)
            hid.assert_called_once_with('PC')
        self.assertTrue(device.closed)
        self.assertEqual(len(device.routing_writes), 1)

    def test_lost_device_after_routing_write_never_reopens_a_different_board(self):
        class LostDongle(ConnectionDongle):
            def get(self, report):
                if self.routing_writes:
                    raise OSError('selected board unplugged')
                return super().get(report)

        device = LostDongle()
        with patch.object(airpods, 'Hid', return_value=device) as hid, \
             contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(airpods.main(['--serial', 'PC', 'connect']), 3)
            hid.assert_called_once_with('PC')
        self.assertIn('selected board unplugged', output.getvalue())
        self.assertTrue(device.closed)
        self.assertEqual(len(device.routing_writes), 1)

    def test_status_does_not_query_policy_or_open_transaction_channel(self):
        device = ConnectionDongle()
        with patch.object(airpods, 'Hid', return_value=device), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(airpods.main(['status']), 0)
        self.assertFalse(device.wire_writes)


if __name__ == '__main__':
    unittest.main()
