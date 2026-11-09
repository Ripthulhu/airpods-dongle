import contextlib
import pathlib
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import airpods_att as att
from audio_diagnostics import RequestRejected, Unsupported


def declaration(decl=0x20, handle=0x29, properties=6, uuid=att.LOUD_SOUND_REDUCTION_UUID):
    return struct.pack('<HBH', decl, properties, handle) + uuid.int.to_bytes(16, 'little')


class Dongle:
    """RPC-boundary fake: ATT effects and fresh replies are independent of HID acceptance."""
    def __init__(self):
        self.status = bytearray(64)
        self.status[:7] = b'\x02APDG\x01\x40'
        self.status[7] = 3
        struct.pack_into('<I', self.status, 8, 42)
        self.status[12:18] = bytes.fromhex('010203040506')
        struct.pack_into('<H', self.status, 42, 0x38)
        fields = ['AirPods', 'A3063', 'Apple', 'serial', '9A1', '', '', '', '', '', '9A1']
        self.metadata = struct.pack('<II', 123, 4) + b'\x04\x00\x04\x00\x1d\x00' + bytes(5) + b'\0'.join(x.encode() for x in fields) + b'\0'
        self.service = b'\x07' + struct.pack('<HH', 0x10, 0x30)
        self.pages = [b'\x09\x15' + declaration()]
        self.requests, self.leases, self.writes = [], [], []
        self.value = b'\x00'
        self.read_values = []
        self.confirm = True
        self.now, self.duration = 0., .01
        self.hook = lambda operation, payload, response: response
        self.release_error = None
        self.renew_error = self.write_error = None

    def get(self, report):
        assert report == 2
        return self.status[:]

    def typed(self, device, payload, generation=None):
        assert device is self and generation == 42 and payload[0] == 10
        action, token = payload[1], struct.unpack_from('<I', payload, 2)[0]
        self.leases.append((action, token))
        if action == 0:
            assert token == 0
            return b'\x01' + struct.pack('<I', 77)
        assert token == 77
        if action == 1 and self.renew_error:
            raise self.renew_error
        if action == 2 and self.release_error:
            raise self.release_error
        return b''

    def rpc(self, device, payload, generation=None, operation=7):
        assert device is self and generation == 42
        self.now += self.duration
        self.requests.append((operation, bytes(payload)))
        if operation == 3:
            assert payload in (b'\x01', b'\x07')
            result = self.metadata if payload == b'\x01' else struct.pack('<II', 5, 6) + b'EQ'
        elif operation == 2:
            result = b''
        else:
            assert operation == 1
            if payload[0] == 6:
                assert payload == b'\x06\x01\x00\xff\xff\x00\x28' + att.SERVICE_UUID.int.to_bytes(16, 'little')
                result = self.service
            elif payload[0] == 8:
                assert payload[-2:] == b'\x03\x28'
                result = self.pages.pop(0) if self.pages else b'\x01\x08' + payload[1:3] + b'\x0a'
            elif payload[0] == 0x0a:
                assert payload == b'\x0a\x29\x00'
                result = b'\x0b' + (self.read_values.pop(0) if self.read_values else self.value)
            else:
                assert payload[:3] in (b'\x52\x29\x00', b'\x12\x29\x00')
                if self.write_error:
                    raise self.write_error
                self.writes.append(bytes(payload))
                if self.confirm:
                    self.value = payload[3:]
                result = b'' if payload[0] == 0x52 else b'\x13'
        return self.hook(operation, payload, result)

    def sleep(self, seconds):
        self.now += seconds


@contextlib.contextmanager
def connected(dongle=None):
    dongle = dongle or Dongle()
    with patch.object(att, 'rpc_request', side_effect=dongle.rpc), \
            patch.object(att, 'typed_request', side_effect=dongle.typed), \
            patch.object(att.time, 'monotonic', side_effect=lambda: dongle.now), \
            patch.object(att.time, 'sleep', side_effect=dongle.sleep):
        yield dongle, att.AttSettings(dongle, 42)


class AirPodsAttTests(unittest.TestCase):
    def test_get_discovers_the_uuid_and_actual_value_handle_under_one_lease(self):
        with connected() as (dongle, client):
            self.assertFalse(client.get_loud_sound_reduction())
            self.assertEqual([(0, 0), (2, 77)], dongle.leases)
            self.assertIn((1, b'\x0a\x29\x00'), dongle.requests)
            self.assertFalse(dongle.writes)

    def test_write_command_uses_byte_zero_or_one_and_exact_fresh_readback(self):
        for before, enabled, value in ((b'\0', True, b'\1'), (b'\1', False, b'\0')):
            with self.subTest(enabled=enabled), connected() as (dongle, client):
                dongle.value = before
                self.assertTrue(client.set_loud_sound_reduction(enabled))
                self.assertEqual([b'\x52\x29\x00' + value], dongle.writes)
                self.assertEqual(3, sum(payload[0] == 0x0a for operation, payload in dongle.requests if operation == 1))
                self.assertEqual((2, 77), dongle.leases[-1])

    def test_same_fresh_value_is_a_noop(self):
        with connected() as (dongle, client):
            self.assertFalse(client.set_loud_sound_reduction(False))
            self.assertFalse(dongle.writes)

    def test_advertised_write_request_requires_its_exact_ack(self):
        with connected() as (dongle, client):
            dongle.pages = [b'\x09\x15' + declaration(properties=0x0a)]
            self.assertTrue(client.set_loud_sound_reduction(True))
            self.assertEqual([b'\x12\x29\x00\x01'], dongle.writes)

    def test_invalid_boolean_reads_never_enable_a_write(self):
        for value in (b'', b'\x02', b'\0\0', b'\1\0', b'\xff'):
            with self.subTest(value=value), connected() as (dongle, client):
                dongle.value = value
                with self.assertRaises(RuntimeError) as error:
                    client.set_loud_sound_reduction(True)
                self.assertNotIsInstance(error.exception, att.Unconfirmed)
                self.assertFalse(dongle.writes)
                self.assertEqual((2, 77), dongle.leases[-1])

    def test_unconfirmed_write_is_never_retried_and_cleanup_preserves_the_error(self):
        with connected() as (dongle, client):
            dongle.confirm = False
            dongle.release_error = RequestRejected(3)
            with self.assertRaisesRegex(att.Unconfirmed, 'No fresh matching'):
                client.set_loud_sound_reduction(True)
            self.assertEqual([b'\x52\x29\x00\x01'], dongle.writes)
            self.assertEqual(7, sum(payload[0] == 0x0a for operation, payload in dongle.requests if operation == 1))

    def test_a_lost_write_receipt_is_uncertain_even_if_it_applied(self):
        with connected() as (dongle, client):
            def lose(operation, payload, response):
                if payload[0] == 0x52:
                    raise OSError('USB receipt lost')
                return response
            dongle.hook = lose
            with self.assertRaisesRegex(att.Unconfirmed, 'may have applied'):
                client.set_loud_sound_reduction(True)
            self.assertEqual(b'\1', dongle.value)
            self.assertEqual(1, len(dongle.writes))

    def test_explicit_preexecution_write_rejections_stay_regular_errors(self):
        for failure in (RequestRejected(1), RequestRejected(2), RequestRejected(4), Unsupported('No support')):
            with self.subTest(failure=str(failure)), connected() as (dongle, client):
                dongle.write_error = failure
                with self.assertRaises(type(failure)) as result:
                    client.set_loud_sound_reduction(True)
                self.assertIs(result.exception, failure)
                self.assertFalse(dongle.writes)

    def test_invalid_command_or_request_completions_are_uncertain_not_confirmation(self):
        for properties, returned in ((6, b'\x13'), (10, b'\x13\0')):
            with self.subTest(properties=properties), connected() as (dongle, client):
                dongle.pages = [b'\x09\x15' + declaration(properties=properties)]
                dongle.hook = lambda operation, payload, response: returned if operation == 1 and payload[0] in (0x12, 0x52) else response
                with self.assertRaises(att.Unconfirmed): client.set_loud_sound_reduction(True)
                self.assertEqual(1, len(dongle.writes))

    def test_owner_change_after_the_single_write_is_uncertain(self):
        with connected() as (dongle, client):
            def replace(operation, payload, response):
                if operation == 1 and payload[0] == 0x52:
                    dongle.status[12] ^= 1
                return response
            dongle.hook = replace
            with self.assertRaises(att.Unconfirmed): client.set_loud_sound_reduction(True)
            self.assertEqual(1, len(dongle.writes))
            self.assertEqual([(0, 0)], dongle.leases)

    def test_malformed_postwrite_readback_is_not_confirmation(self):
        with connected() as (dongle, client):
            dongle.read_values = [b'\0', b'\0', b'\1\0']
            with self.assertRaises(att.Unconfirmed):
                client.set_loud_sound_reduction(True)
            self.assertEqual(1, len(dongle.writes))

    def test_baseline_change_aborts_before_the_write(self):
        with connected() as (dongle, client):
            dongle.read_values = [b'\0', b'\1']
            with self.assertRaisesRegex(RuntimeError, 'changed before') as error:
                client.set_loud_sound_reduction(True)
            self.assertNotIsInstance(error.exception, att.Unconfirmed)
            self.assertFalse(dongle.writes)

    def test_owner_changes_before_write_cannot_cross_to_another_peer_or_session(self):
        for changed in ('peer', 'generation', 'microphone', 'expired'):
            with self.subTest(changed=changed), connected() as (dongle, client):
                def replace(operation, payload, response):
                    if operation == 1 and payload[0] == 0x0a:
                        if changed == 'peer': dongle.status[12] ^= 1
                        elif changed == 'generation': struct.pack_into('<I', dongle.status, 8, 43)
                        elif changed == 'microphone': dongle.status[7] |= 16
                        else: dongle.now = 31
                    return response
                dongle.hook = replace
                with self.assertRaises(RuntimeError) as error:
                    client.set_loud_sound_reduction(True)
                self.assertNotIsInstance(error.exception, att.Unconfirmed)
                self.assertFalse(dongle.writes)
                self.assertEqual([(0, 0), (2, 77)] if changed in ('microphone', 'expired') else [(0, 0)], dongle.leases)

    def test_renewal_uses_the_same_token_without_reacquiring(self):
        with connected() as (dongle, client):
            dongle.duration = .8
            self.assertTrue(client.set_loud_sound_reduction(True))
            self.assertIn((1, 77), dongle.leases)
            self.assertEqual(1, dongle.leases.count((0, 0)))
            self.assertEqual((2, 77), dongle.leases[-1])

    def test_failed_renewal_never_reacquires_or_sends_a_write(self):
        with connected() as (dongle, client):
            dongle.duration, dongle.renew_error = .8, RequestRejected(3)
            with self.assertRaises(RequestRejected): client.set_loud_sound_reduction(True)
            self.assertEqual(1, dongle.leases.count((0, 0)))
            self.assertEqual((2, 77), dongle.leases[-1])
            self.assertFalse(dongle.writes)

    def test_unknown_model_missing_lease_and_stale_generation_fail_closed(self):
        for changed in ('model', 'lease', 'generation'):
            dongle = Dongle()
            if changed == 'model': dongle.metadata = dongle.metadata.replace(b'A3063', b'A2084')
            if changed == 'lease': dongle.status[42] &= ~32
            if changed == 'generation': struct.pack_into('<I', dongle.status, 8, 43)
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                with connected(dongle) as (_, client): client.set_loud_sound_reduction(True)
            self.assertFalse(dongle.writes)

    def test_malformed_or_duplicate_discovery_never_uses_a_fixed_handle(self):
        pages = [b'\x09\x15' + declaration()[:-1], b'\x09\x15' + declaration(handle=0x31),
                 b'\x09\x15' + declaration(0x10, 0x11) + declaration(0x12, 0x13), b'\x09\x07',
                 b'\x01\x08\xff\xff\x0a']
        for page in pages:
            with self.subTest(page=page), connected() as (dongle, client):
                dongle.pages = [page]
                with self.assertRaises(RuntimeError): client.set_loud_sound_reduction(True)
                self.assertFalse(dongle.writes)

    def test_missing_or_unreadable_characteristic_is_unavailable(self):
        for properties in (None, 4):
            with self.subTest(properties=properties), connected() as (dongle, client):
                dongle.pages = [] if properties is None else [b'\x09\x15' + declaration(properties=properties)]
                with self.assertRaises(Unsupported): client.get_loud_sound_reduction()
                self.assertFalse(dongle.writes)

    def test_a_readonly_characteristic_is_readable_but_not_writable(self):
        with connected() as (dongle, client):
            dongle.pages = [b'\x09\x15' + declaration(properties=2)]
            with self.assertRaises(Unsupported): client.set_loud_sound_reduction(True)
            self.assertFalse(dongle.writes)

    def test_generic_helpers_share_one_lease_and_reject_retired_characteristics(self):
        with connected() as (dongle, client):
            with client.workflow():
                self.assertEqual('A3063', client.model())
                age, revision, packet = client.cached(1)
                self.assertEqual((123, 4), (age, revision))
                self.assertEqual((5, 6, b'EQ'), client.cached(7))
                with self.assertRaises(ValueError): client.cached(6)
                characteristic = client.discover(att.LOUD_SOUND_REDUCTION_UUID)
                self.assertEqual(b'\0', client.read(characteristic))
                self.assertEqual(b'', client.request(b'\x04\x00\x04\x00\x0f\x00' + bytes(4)))
            with self.assertRaises(RuntimeError): client.read(characteristic)
            self.assertEqual([(0, 0), (2, 77)], dongle.leases)

    def test_all_known_pro2_and_pro3_model_numbers_use_discovery(self):
        for model in sorted(att.LSR_MODELS):
            with self.subTest(model=model), connected() as (dongle, client):
                dongle.metadata = dongle.metadata.replace(b'A3063', model.encode())
                self.assertFalse(client.get_loud_sound_reduction())
                self.assertIn((1, b'\x0a\x29\x00'), dongle.requests)

    def test_nonboolean_input_is_rejected_without_a_lease_or_write(self):
        with connected() as (dongle, client):
            for value in (0, 1, 'on', None):
                with self.assertRaises(ValueError): client.set_loud_sound_reduction(value)
            self.assertFalse(dongle.leases)
            self.assertFalse(dongle.requests)


if __name__ == '__main__':
    unittest.main()
