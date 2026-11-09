import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from audio_diagnostics import rpc_request, typed_request


class RpcDongle:
    def __init__(self, reply=b''):
        self.status = bytearray(64)
        struct.pack_into('<I', self.status, 8, 42)
        self.report = bytearray(64)
        self.payload = bytearray()
        self.reply = reply
        self.writes = []
        self.change_after_fragment = False

    def get(self, report):
        return (self.status if report == 2 else self.report)[:]

    def set(self, data):
        self.writes.append(bytes(data))
        self.report = bytearray(data)
        offset = struct.unpack_from('<H', data, 18)[0]
        if data[7] in (1, 2, 3, 7):
            assert offset == len(self.payload)
            self.operation = data[7]
            self.payload.extend(data[22:22 + data[20]])
            self.report[21] = 1
            struct.pack_into('<H', self.report, 18, len(self.payload))
            if self.change_after_fragment:
                struct.pack_into('<I', self.status, 8, 43)
            return
        assert data[7] in (4, 6)
        self.report[7] = self.operation
        self.report[21] = 3
        body = self.reply[offset:offset + 42]
        struct.pack_into('<HHB', self.report, 16, len(self.reply), offset, len(body))
        self.report[22:] = bytes(42)
        self.report[22:22 + len(body)] = body


class SettingsRpcTest(unittest.TestCase):
    def test_fragmented_raw_write_commits_once_and_pages_reply(self):
        for operation in (1, 2):
            data = bytes(range(256)) * 2
            dongle = RpcDongle(bytes(range(123)))
            self.assertEqual(rpc_request(dongle, data, 42, operation), dongle.reply)
            self.assertEqual(dongle.payload, data)
            self.assertEqual(sum(p[7] == 6 for p in dongle.writes), 1)
            self.assertEqual([struct.unpack_from('<H', p, 18)[0] for p in dongle.writes if p[7] == operation],
                             list(range(0, 512, 42)))

    def test_session_change_aborts_without_commit_or_retry(self):
        dongle = RpcDongle()
        dongle.change_after_fragment = True
        with self.assertRaisesRegex(RuntimeError, 'session changed'):
            rpc_request(dongle, bytes(104), 42, 1)
        self.assertEqual(len(dongle.writes), 1)
        self.assertFalse(any(p[7] == 6 for p in dongle.writes))

    def test_invalid_operation_or_size_never_sends(self):
        dongle = RpcDongle()
        for data, operation in ((b'', 1), (bytes(513), 1), (b'1', 5), (b'1', 6)):
            with self.assertRaises(ValueError):
                rpc_request(dongle, data, operation=operation)
        with self.assertRaises(ValueError):
            typed_request(dongle, bytes(43))
        self.assertFalse(dongle.writes)


if __name__ == '__main__':
    unittest.main()
