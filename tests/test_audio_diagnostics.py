import importlib.util
import pathlib
import struct
import sys
import unittest
from unittest.mock import patch, mock_open

path = pathlib.Path(__file__).resolve().parents[1] / 'tools/audio_diagnostics.py'
spec = importlib.util.spec_from_file_location('audio_diagnostics', path)
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)
sys.modules['audio_diagnostics'] = diagnostics
profile_spec = importlib.util.spec_from_file_location('audio_profile', path.with_name('audio_profile.py'))
profile = importlib.util.module_from_spec(profile_spec)
profile_spec.loader.exec_module(profile)


class FakeHid:
    def __init__(self, payload, busy=False, unsupported=False):
        self.payload = payload
        self.report = bytearray(64)
        self.report[:7] = bytes((3,)) + diagnostics.HEADER
        struct.pack_into('<II', self.report, 8, 42, 100)
        self.report[21] = 1 if busy else 3
        self.writes = []
        self.unsupported = unsupported

    def get(self, report):
        if report == 3:
            return self.report[:]
        output = bytearray(64)
        output[:7] = bytes((2,)) + diagnostics.HEADER
        struct.pack_into('<I', output, 8, 42)
        return output

    def set(self, data):
        self.writes.append(bytes(data))
        self.report = data[:]
        if data[7] == 7:
            assert data[20] == 1 and data[22] in (5, 9, 11, 13, 15)
            self.report[21] = 1
            struct.pack_into('<H', self.report, 18, 1)
        else:
            assert data[7] in (4, 6)
            if self.unsupported:
                self.report[21] = 4
                self.report[22] = 7
                return
            offset = 0 if data[7] == 6 else struct.unpack_from('<H', data, 18)[0]
            page = self.payload[offset:offset + 42]
            self.report[21] = 3
            struct.pack_into('<HHB', self.report, 16, len(self.payload), offset, len(page))
            self.report[22:22 + len(page)] = page


class AudioDiagnosticsTests(unittest.TestCase):
    def test_inspection_is_disabled_after_capture_error(self):
        with patch.object(sys,'argv',['audio_diagnostics.py','--inspect','--seconds','1','--output','capture.jsonl']), \
             patch.object(diagnostics,'Hid') as hid, \
             patch.object(diagnostics,'sample',side_effect=RuntimeError('lost audio')), \
             patch.object(diagnostics,'_transaction') as transact, patch('builtins.open',mock_open()):
            with self.assertRaisesRegex(RuntimeError,'lost audio'):diagnostics.main()
            self.assertEqual([c.args[1] for c in transact.call_args_list],[bytes((14,1)),bytes((14,0))])
            hid.return_value.close.assert_called_once()
    def test_audio_inspect_layout(self):
        data=bytearray(400);data[:4]=bytes((1,3,0,4));data[68:72]=b'ABCD'
        struct.pack_into('<I',data,8+10*4,2);data[80:82]=b'EF'
        decoded=diagnostics.decode_inspect(data)
        self.assertEqual(decoded['asc_hex'],'41424344')
        self.assertEqual(decoded['au_hex'],'4546')
        self.assertEqual(diagnostics.feature(FakeHid(data),13),data)
        with self.assertRaises(RuntimeError):diagnostics.decode_inspect(data[:-1])
    def test_read_only_pagination(self):
        payload = bytes(range(256)) * 2
        fake = FakeHid(payload)
        self.assertEqual(diagnostics.feature(fake, 11), payload)
        self.assertEqual(len(fake.writes), 14)

    def test_refuses_settings_and_existing_transaction(self):
        fake = FakeHid(bytes(20))
        with self.assertRaises(ValueError):
            diagnostics.feature(fake, 4)
        self.assertFalse(fake.writes)
        fake = FakeHid(bytes(20), busy=True)
        with self.assertRaises(RuntimeError):
            diagnostics.feature(fake, 9)
        self.assertFalse(fake.writes)

    def test_old_firmware_unsupported(self):
        with self.assertRaises(diagnostics.Unsupported):
            diagnostics.feature(FakeHid(bytes(20), unsupported=True), 11)

    def test_trace_layout_and_bounds(self):
        data = bytearray(512)
        data[:3] = bytes((1, 16, 1))
        struct.pack_into('<I', data, 32 + 22*4, 2)
        struct.pack_into('<4I', data, 128, 1234, 6, 42, 0)
        decoded = diagnostics.decode_trace(data)
        self.assertEqual(decoded['stats']['usb_alternate_setting'], 2)
        self.assertEqual(decoded['events'][0]['kind'], 'controller_flush')
        for bad in (data[:-1], bytes(512)):
            with self.assertRaises(RuntimeError):
                diagnostics.decode_trace(bad)
        data[2] = 25
        with self.assertRaises(RuntimeError):
            diagnostics.decode_trace(data)

    def test_profiling_restores_game_mode_after_capture_failure(self):
        original = dict(game_requested=True, audio_flags=10,
                        trace=dict(stats=dict(ble_scan_requested=1)))
        with patch.object(sys, 'argv', ['audio_profile.py', '--game', 'off', '--output', 'capture.jsonl']), \
             patch.object(profile, 'Hid') as hid, \
             patch.object(profile, 'sample', side_effect=[original, RuntimeError('lost USB'), original]), \
             patch.object(profile, '_transaction') as transact, patch('builtins.open', mock_open()):
            with self.assertRaisesRegex(RuntimeError, 'lost USB'):
                profile.main()
            self.assertEqual([call.args[1] for call in transact.call_args_list], [bytes((4, 0)), bytes((4, 1))])
            hid.return_value.close.assert_called_once()

    def test_phone_counters_and_invalid_layouts(self):
        data = bytearray((1, 3, 0xff, 12)) + bytearray(48)
        values = list(range(12))
        values[9] = 1
        struct.pack_into('<12I', data, 4, *values)
        decoded = diagnostics.decode_phone(data)
        self.assertEqual(decoded['submit_failures'], 3)
        self.assertEqual(decoded['queued_packets'], 8)
        self.assertEqual(decoded['max_decode_us'], 11)
        self.assertEqual(decoded['flags'], 0xff)
        for bad in (data[:-1], bytes(52), bytearray((1, 4, 0, 12)) + data[4:]):
            with self.assertRaises(RuntimeError):
                diagnostics.decode_phone(bad)
        struct.pack_into('<I', data, 40, 2)
        with self.assertRaises(RuntimeError):
            diagnostics.decode_phone(data)

    def test_phone_feature_is_read_only(self):
        with patch.object(diagnostics, '_transaction', return_value=b'phone') as transact:
            self.assertEqual(diagnostics.feature(None, 15), b'phone')
            transact.assert_called_once_with(None, b'\x0f')
        with self.assertRaises(ValueError):
            diagnostics.feature(None, 16)

    def test_phone_v2_gaps_and_pagination(self):
        data = bytearray((2, 3, 0xff, 15)) + bytearray(60)
        values = list(range(15))
        values[9] = 1
        struct.pack_into('<15I', data, 4, *values)
        decoded = diagnostics.decode_phone(data)
        self.assertEqual(decoded['max_media_gap_ms'], 12)
        self.assertEqual(decoded['max_pcm_gap_ms'], 13)
        self.assertEqual(decoded['timestamp_discontinuities'], 14)
        self.assertEqual(diagnostics.feature(FakeHid(data), 15), data)
        for bad in (b'', data[:-1], bytearray((1, 3, 0xff, 15)) + data[4:],
                    bytearray((2, 3, 0xff, 12)) + data[4:],
                    bytearray((2, 4, 0xff, 15)) + data[4:]):
            with self.assertRaises(RuntimeError):
                diagnostics.decode_phone(bad)
        struct.pack_into('<I', data, 40, 2)
        with self.assertRaises(RuntimeError):
            diagnostics.decode_phone(data)

    def test_phone_capture_handles_older_firmware(self):
        audio, perf = bytearray(20), bytearray(64)
        audio[0] = perf[0] = 1
        def read(_device, command):
            if command == 15:
                raise diagnostics.Unsupported('Old firmware')
            return {9: audio, 5: perf}[command]
        with patch.object(diagnostics, 'feature', side_effect=read):
            result = diagnostics.sample(None, trace=False, phone=True)
        self.assertIs(result['phone_supported'], False)
