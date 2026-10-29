import importlib.util
import pathlib
import struct
import sys
import unittest
from types import SimpleNamespace
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
            assert ((data[20] == 1 and data[22] in (5, 9, 11, 13, 15, 19)) or
                    (data[20] == 2 and data[22] == 18 and data[23] in (0, 1)))
            self.report[21] = 1
            struct.pack_into('<H', self.report, 18, data[20])
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
    def test_radio_read_is_cached_bounded_and_preserves_signed_units(self):
        data=bytearray(128);data[:4]=bytes((1,128,7,31));data[64:68]=bytes((1,9,3,0))
        struct.pack_into('<IIH',data,4,10000,7,0x123)
        data[14:21]=bytes((2,8,244,200,252,9,1));data[21]=0x55
        struct.pack_into('<5I',data,32,200,400,600,0xffffffff,800)
        struct.pack_into('<II',data,52,12,3400)
        struct.pack_into('<15I',data,68,192000,256000,174000,3,0,9000,3,4,1000,
                         128000,0,0,8,12,7)
        fake=FakeHid(data)
        result=diagnostics.sample(fake,radio=True)
        self.assertEqual(set(result),{'time','radio'})
        radio=result['radio']
        self.assertEqual(radio['rssi_relative_db'],-12)
        self.assertEqual(radio['tx_power_dbm'],-4)
        self.assertEqual(radio['max_tx_power_dbm'],9)
        self.assertIsNone(radio['age_ms']['max_tx_power'])
        self.assertEqual(radio['handle'],0x123)
        self.assertEqual(radio['adaptive']['current_bps'],192000)
        self.assertEqual(radio['adaptive']['target_bps'],174000)
        self.assertEqual(radio['adaptive']['reason'],'pressure')
        self.assertTrue(radio['adaptive']['variable'])
        self.assertEqual([p[22] for p in fake.writes if p[7]==7],[19])
        data[3]=0
        radio=diagnostics.decode_radio(data)
        self.assertIsNone(radio['rssi_relative_db'])
        self.assertIsNone(radio['afh_map'])
        self.assertIsNone(radio['afh_mode'])
        for bad in (data[:-1], bytes(128)):
            with self.assertRaises(RuntimeError):diagnostics.decode_radio(bad)
        for offset,value in ((0,2),(1,127),(2,8),(3,32),(14,32),(15,32),(64,2),(65,16),(66,7)):
            bad=data[:];bad[offset]=value
            with self.assertRaises(RuntimeError):diagnostics.decode_radio(bad)
        with self.assertRaises(diagnostics.Unsupported):
            diagnostics.sample(FakeHid(data,unsupported=True),radio=True)

    def test_linux_selects_serial_across_audio_and_control_personalities(self):
        devices = {10: (0xcafe, 0x4015, 'PC'), 11: (0xcafe, 0x4005, 'PHONE'),
                   12: (0xcafe, 0x4016, 'OTHER')}
        def ioctl(fd, command, data, mutable):
            vid, pid, serial = devices[fd]
            self.assertTrue(mutable)
            if command == 0x80084803:
                struct.pack_into('<IHH', data, 0, 3, vid, pid)
            else:
                self.assertEqual(command, 0x81004808)
                data[:len(serial)] = serial.encode()
        with patch.dict(sys.modules, {'fcntl': SimpleNamespace(ioctl=ioctl)}), \
             patch.multiple(diagnostics.os, name='posix', O_CLOEXEC=0, create=True), \
             patch.object(diagnostics.glob, 'glob', return_value=['a', 'b', 'c']), \
             patch.object(diagnostics.os, 'open', side_effect=lambda path, _: {'a':10,'b':11,'c':12}[path]), \
             patch.object(diagnostics.os, 'close') as close:
            device = diagnostics.Hid('PHONE')
            self.assertEqual(device.handle, 11)
            self.assertEqual([call.args[0] for call in close.call_args_list], [10, 12])
            device.close()
            self.assertEqual(close.call_args.args, (11,))
            close.reset_mock()
            with self.assertRaisesRegex(RuntimeError, 'exactly one matching'):
                diagnostics.Hid()
            self.assertEqual(sorted(call.args[0] for call in close.call_args_list), [10, 11, 12])
            close.reset_mock()
            device = diagnostics.Hid('PC')
            self.assertEqual(device.handle, 10)
            device.close()

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

    def test_bounded_flush_comparison_command(self):
        for enabled in (0, 1):
            fake = FakeHid(b'')
            self.assertEqual(diagnostics._transaction(fake, bytes((18, enabled))), b'')
            self.assertEqual(fake.writes[0][22:24], bytes((18, enabled)))
        for malformed in (b'\x12', b'\x12\x02', b'\x12\x01\x00'):
            fake = FakeHid(b'')
            with self.assertRaises(ValueError):
                diagnostics._transaction(fake, malformed)
            self.assertFalse(fake.writes)
        with self.assertRaises(diagnostics.Unsupported):
            diagnostics._transaction(FakeHid(b'', unsupported=True), bytes((18, 1)))

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
