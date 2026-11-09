import contextlib
import hashlib
import math
import pathlib
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import airpods_hearing as hearing
from airpods_att import Unconfirmed
from audio_diagnostics import Unsupported


def profile(clinical=False, enabled=False, left=None, right=None):
    values = [0.] * 26
    values[0] = float(enabled)
    values[1:9] = left or [5., 10., 15., 20., 25., 30., 35., 40.]
    values[13:21] = right or [10., 15., 20., 25., 30., 35., 40., 45.]
    values[10], values[22], values[25] = -0., -0., .25
    data = struct.pack('<26f', *values)
    return b'\x02\x02\x64\0' + data[4:] if clinical else data


def changed(data, index, value):
    result = bytearray(data)
    struct.pack_into('<f', result, index * 4, value)
    return bytes(result)


def records(ha=False, phone=False, media=False, enrolled=True, gate=None, pme_enrollment=2):
    gate = ha or phone or media if gate is None else gate
    h = bytes.fromhex('0400040009002c') + bytes((1 if enrolled else 2, 1 if ha else 2, 0, 0))
    g = bytes.fromhex('04000400090033') + bytes((1 if gate else 2, 0, 0, 0))
    p = hearing.PME_HEADER + bytes((pme_enrollment, 1 if phone else 2, 1 if media else 2)) + hearing.STOCK_CURVES[0]
    return {slot: (0, slot + 10, packet) for slot, packet in zip((2, 3, 4), (h, g, p))}


class FakeAtt:
    """Real profile bytes and independent raw reports; the CLI cannot self-confirm sends."""
    def __init__(self):
        self.profiles = {hearing.FITTING: profile(True), hearing.TRANSPARENCY: profile()}
        self.reports = records()
        self.reports[7] = (0, 17, hearing.EQ_HEADER + bytes((1, 30, 50, 70)))
        self.writes, self.requests = [], []
        self.cached_calls, self.read_calls = {}, {}
        self.read_queue = {}
        self.cache_hook = lambda slot, count, value: value
        self.report_hook = lambda slot, value: value
        self.confirm_att = self.confirm_reports = self.advance_revision = True
        self.preserve_pme_enrollment = True
        self.current, self.active, self.write_attempted = True, False, False
        self.model_number = 'A3063'
        self.now = 0.
        self.entered = self.exited = 0
        self.after_write = lambda: None
        self.after_request = lambda: None
        self.after_read = lambda characteristic, count: None

    def check(self):
        if not self.active or not self.current:
            raise RuntimeError('The original settings owner is no longer current')

    @contextlib.contextmanager
    def workflow(self):
        self.active, self.write_attempted = True, False
        self.entered += 1
        try:
            yield self
        finally:
            self.exited += 1
            self.active = False

    def model(self):
        self.check()
        return self.model_number

    def discover(self, identifier):
        self.check()
        if identifier not in self.profiles:
            raise Unsupported('Characteristic absent')
        return identifier

    def read(self, characteristic):
        self.check()
        self.read_calls[characteristic] = self.read_calls.get(characteristic, 0) + 1
        queued = self.read_queue.get(characteristic, [])
        value = queued.pop(0) if queued else self.profiles[characteristic]
        if isinstance(value, Exception):
            raise value
        self.after_read(characteristic, self.read_calls[characteristic])
        return value

    def write(self, characteristic, value):
        self.check()
        self.write_attempted = True
        self.writes.append((characteristic, bytes(value)))
        if self.confirm_att:
            self.profiles[characteristic] = bytes(value)
        self.after_write()

    def cached(self, slot):
        self.check()
        count = self.cached_calls[slot] = self.cached_calls.get(slot, 0) + 1
        return self.cache_hook(slot, count, self.reports.get(slot))

    def request(self, packet):
        self.check()
        self.write_attempted = True
        self.requests.append(bytes(packet))
        slot = 2 if packet[4] == 9 and packet[6] == 0x2c else 3 if packet[4] == 9 and packet[6] == 0x33 else 4 if packet[4] == 0x53 else 7 if packet[4] == 0x63 else None
        if self.confirm_reports and slot is not None:
            age, revision, _ = self.reports[slot]
            reported = bytes(packet)
            if slot == 4 and self.preserve_pme_enrollment:
                reported = reported[:9] + self.reports[slot][2][9:10] + reported[10:]
            value = (0, revision + int(self.advance_revision), reported)
            self.reports[slot] = self.report_hook(slot, value)
        self.after_request()
        return b''

    def sleep(self, seconds):
        self.now += seconds


@contextlib.contextmanager
def fixture(fake=None):
    fake = fake or FakeAtt()
    with patch.object(hearing, 'AttSettings', return_value=fake), \
            patch.object(hearing.time, 'monotonic', side_effect=lambda: fake.now), \
            patch.object(hearing.time, 'sleep', side_effect=fake.sleep):
        yield fake, hearing.HearingSettings(object(), 42)


class HearingSchemaTests(unittest.TestCase):
    def test_mode_plans_preserve_enrollment_curves_and_independent_shared_gate_users(self):
        before = records(phone=True, media=True)
        packets, expected = hearing.mode_plan(list(before.values()), 'hearing-aid', True)
        self.assertEqual((True, True, True, True), expected)
        self.assertEqual([bytes.fromhex('0400040009002c01010000'), bytes.fromhex('0400040009000d03000000')], packets)
        self.assertEqual(hearing.STOCK_CURVES[0], before[4][2][12:])
        packets, expected = hearing.mode_plan(list(records(ha=True, phone=True, media=True).values()), 'media-assist', False)
        self.assertEqual((True, True, False, False), expected)
        self.assertEqual(1, len(packets))
        self.assertEqual(hearing.PME_HEADER + b'\x02\x02\x02' + hearing.STOCK_CURVES[0], packets[0])

    def test_enabling_requires_enrollment_consistent_gate_and_complete_finite_reports(self):
        for state in (records(enrolled=False), records(gate=True)):
            with self.subTest(state=state), self.assertRaises(RuntimeError):
                hearing.mode_plan(list(state.values()), 'hearing-aid', True)
        for slot, packet in ((2, b'bad'), (3, bytes.fromhex('0400040009003301000001')),
                             (4, hearing.PME_HEADER + b'\x02\x02\x02' + struct.pack('<f', math.nan) + bytes(124))):
            state = records()
            state[slot] = (0, 1, packet)
            with self.subTest(slot=slot), self.assertRaises(RuntimeError): hearing.modes(list(state.values()))

    def test_complete_profiles_reject_invalid_prefixes_ranges_and_nonfinite_values(self):
        for clinical in (False, True):
            baseline = profile(clinical)
            invalid = [baseline[:-1], baseline + b'\0', changed(baseline, 9, 3), changed(baseline, 10, 1.01),
                       changed(baseline, 11, .5), changed(baseline, 12, -1), changed(baseline, 25, 1.01),
                       changed(baseline, 1, math.nan), changed(baseline, 21, math.inf)]
            invalid += [b'\x02\x01\x64\0' + baseline[4:], b'\x02\x02\x61\0' + baseline[4:]] if clinical else [changed(baseline, 0, 2)]
            if clinical: invalid += [changed(baseline, 1, 90.1), changed(baseline, 20, -1)]
            for value in invalid:
                with self.subTest(clinical=clinical, value=value[:4]), self.assertRaises(RuntimeError): hearing.validate_record(value, clinical)
        self.assertEqual(25, len(hearing.validate_record(profile()[:-4])))
        with self.assertRaises(Unsupported): hearing.patch_record(profile()[:-4], 'own-voice', .5)

    def test_adjustments_change_only_the_named_fields_and_keep_signed_zero_and_curves(self):
        for clinical in (False, True):
            original = profile(clinical)
            requested = hearing.patch_record(original, 'noise-reduction', .75, clinical)
            expected = changed(changed(original, 12, .75), 24, .75)
            self.assertEqual(expected, requested)
            self.assertEqual(original[40:44], requested[40:44])
            self.assertEqual(original[4:36], requested[4:36])
            self.assertEqual(original[52:84], requested[52:84])
            self.assertEqual(original, hearing.patch_record(original, 'tone', 0., clinical))

    def test_amplification_and_balance_follow_the_native_two_ear_formulas(self):
        result = hearing.patch_record(profile(True), 'balance', 1., True)
        self.assertEqual((-.5, .5), tuple(hearing.floats(result[4:])[i - 1] for i in (9, 21)))
        result = hearing.patch_record(profile(), 'balance', -1.25)
        values = hearing.floats(result)
        self.assertEqual((1.25, 0.), (values[9], values[21]))

    def test_audiogram_normalization_uses_better_ear_threshold_and_missing_upper_values(self):
        left, right = [0, 0, 10, 20, 30, 40, 0, 50], [10, 0, 20, 30, 40, 50, 0, 60]
        self.assertEqual(struct.pack('<8f', 5, 55, 15, 25, 35, 45, 55, 55) * 4,
                         hearing.audiogram_curves(profile(True, left=left, right=right)))
        lower, higher = [5, 10, 10, 10, 10, 10, 10, 20], [20, 25, 25, 25, 25, 25, 25, 35]
        for left, right in ((lower, higher), (higher, lower)):
            self.assertEqual(struct.pack('<8f', *lower) * 4, hearing.audiogram_curves(profile(True, left=left, right=right)))
        with self.assertRaises(RuntimeError): hearing.audiogram_curves(b'\x02\x00\x64\0' + profile(True)[4:])

    def test_all_nine_stock_blocks_match_the_captured_native_fixtures(self):
        # SHA256 of the nine anonymous stock blocks in LibrePods core Accommodations.kt.
        expected = ('d021353c1d99529dfb6a463b17c0c119f39a64933c6c4640f5ef306456ef29bd',
                    'eeeacde7d2f20714f4cfcbfb4db5aa7e08c1eef48080aa5a1c4a111e1658c6d1',
                    'd2d16c8f23e5ecb0b184d4290271053908f15dd78a8e622d7e51156ee325c856',
                    'de0ad08949f66bc89b1b8767f1175898ab83e31586bcde0a7c66c036fa1e7782',
                    '56be48572acdea6946c438e7b4ddc6cbbde489b7ff3eddfb2fe164fd6c3067ef',
                    '803b571a2cb937f7127eafc2c7247c275957454e0b89f3ed6c31e9425d5f2b77',
                    '464f9be37f92023891c55a8bf399d4603e84d27394bbc515d5c71e151f29029b',
                    '227dba5c9c8929d1640859abe8a8bf6d07ad83d3d5f8a9222f739ce7277e4164',
                    '3d1ac0c060a2dc2b8ae89f04c7a8fdf527d2feb80fe2ed78280ae347c304ac7e')
        self.assertEqual(expected, tuple(hashlib.sha256(value).hexdigest() for value in hearing.STOCK_CURVES))
        self.assertTrue(all(len(value) == 128 for value in hearing.STOCK_CURVES))

    def test_named_input_validation_rejects_nonfinite_fractional_eq_and_out_of_range_values(self):
        for name, value in (('hearing-tone', 'nan'), ('hearing-balance', 'inf'), ('transparency-amplification', '2.1'),
                            ('hearing-amplification', '-1.6'), ('eq-high', '50.5'), ('eq-low', '101'), ('media-assist', 'true')):
            with self.subTest(name=name, value=value), self.assertRaises(ValueError): hearing.encode(name, value)


class HearingWorkflowTests(unittest.TestCase):
    def test_opening_and_getting_all_supported_settings_sends_nothing(self):
        with fixture() as (fake, client):
            for name in hearing.PARAMETERS:
                client.get(name)
            self.assertFalse(fake.writes)
            self.assertFalse(fake.requests)
            self.assertEqual(fake.entered, fake.exited)

    def test_clinical_mode_enable_preserves_fitting_and_full_disable_keeps_enrollment(self):
        with fixture() as (fake, client):
            original = fake.profiles[hearing.FITTING]
            self.assertTrue(client.set('hearing-aid', True))
            self.assertTrue(client.get('hearing-aid'))
            self.assertTrue(client.set('hearing-aid', False))
            self.assertFalse(client.get('hearing-aid'))
            self.assertEqual(original, fake.profiles[hearing.FITTING])
            self.assertEqual(1, fake.reports[2][2][7])
            self.assertEqual(2, fake.reports[3][2][7])
            self.assertFalse(fake.writes)

    def test_media_assist_off_does_not_turn_off_hearing_aid_or_the_shared_gate(self):
        with fixture() as (fake, client):
            fake.reports.update(records(ha=True, phone=True, media=True))
            self.assertTrue(client.set('media-assist', False))
            self.assertTrue(client.get('hearing-aid'))
            self.assertEqual(1, len(fake.requests))
            self.assertEqual(0x53, fake.requests[0][4])
            self.assertEqual(1, fake.reports[3][2][7])

    def test_invalid_model_missing_reports_unwritable_fitting_and_custom_transparency_block_enable(self):
        for invalid in ('unknown', 'wrong-prefix', 'long-model', 'missing', 'unwritable', 'custom'):
            with self.subTest(invalid=invalid), fixture() as (fake, client):
                if invalid in ('unknown', 'wrong-prefix', 'long-model'): fake.model_number = {'unknown': 'A2084', 'wrong-prefix': 'B3063', 'long-model': 'A03063'}[invalid]
                elif invalid == 'missing': fake.reports[3] = None
                elif invalid == 'unwritable': fake.profiles[hearing.FITTING] = b'\x02\0\x64\0' + profile(True)[4:]
                else: fake.profiles[hearing.TRANSPARENCY] = profile(enabled=True)
                with self.assertRaises(RuntimeError): client.set('hearing-aid', True)
                self.assertFalse(fake.requests)
                self.assertFalse(fake.writes)

    def test_legacy_edits_do_not_cross_live_clinical_paths_but_custom_off_is_allowed(self):
        with fixture() as (fake, client):
            fake.reports.update(records(ha=True))
            for name, value in (('headphone-accommodations', True), ('transparency-tone', .25), ('custom-transparency', True)):
                with self.subTest(name=name), self.assertRaises(RuntimeError): client.set(name, value)
            self.assertFalse(fake.requests)
            self.assertFalse(fake.writes)
            fake.profiles[hearing.TRANSPARENCY] = profile(enabled=True)
            self.assertTrue(client.set('custom-transparency', False))
            self.assertEqual(0., hearing.floats(fake.profiles[hearing.TRANSPARENCY])[0])

    def test_record_edit_is_one_write_with_preserved_curves_and_fresh_readback(self):
        with fixture() as (fake, client):
            baseline = fake.profiles[hearing.FITTING]
            self.assertTrue(client.set('hearing-tone', .5))
            self.assertEqual(1, len(fake.writes))
            self.assertEqual(hearing.patch_record(baseline, 'tone', .5, True), fake.writes[0][1])
            self.assertFalse(client.set('hearing-tone', .5))
            self.assertEqual(1, len(fake.writes))

    def test_external_prewrite_profile_or_report_changes_send_nothing_and_are_not_uncertain(self):
        for invalidation in ('profile', 'reports'):
            with self.subTest(invalidation=invalidation), fixture() as (fake, client):
                if invalidation == 'profile':
                    original = fake.profiles[hearing.FITTING]
                    fake.read_queue[hearing.FITTING] = [original, changed(original, 10, -.5)]
                else:
                    fake.cache_hook = lambda slot, count, value: (value[0], value[1] + 1, value[2]) if slot == 2 and count > 1 else value
                with self.assertRaises(RuntimeError) as result: client.set('hearing-tone', .5)
                self.assertNotIsInstance(result.exception, Unconfirmed)
                self.assertFalse(fake.writes)
                self.assertFalse(fake.requests)

    def test_mode_change_after_the_last_baseline_read_blocks_fitting_and_transparency_sends(self):
        for name, characteristic in (('hearing-tone', hearing.FITTING), ('transparency-tone', hearing.TRANSPARENCY)):
            with self.subTest(name=name), fixture() as (fake, client):
                def change_mode(selected, count):
                    if selected == characteristic and count == 2:
                        age, revision, packet = fake.reports[3]
                        fake.reports[3] = (age, revision + 1, packet[:7] + b'\x01' + packet[8:])
                fake.after_read = change_mode
                with self.assertRaisesRegex(RuntimeError, 'modes changed during') as result:
                    client.set(name, .5)
                self.assertNotIsInstance(result.exception, Unconfirmed)
                self.assertFalse(fake.writes)
                self.assertFalse(fake.requests)

    def test_mode_change_after_a_record_send_is_uncertain_despite_matching_readback(self):
        for name in ('hearing-tone', 'transparency-tone'):
            with self.subTest(name=name), fixture() as (fake, client):
                def change_mode():
                    age, revision, packet = fake.reports[3]
                    fake.reports[3] = (age, revision + 1, packet[:7] + b'\x01' + packet[8:])
                fake.after_write = change_mode
                with self.assertRaisesRegex(Unconfirmed, 'modes changed during'):
                    client.set(name, .5)
                self.assertEqual(1, len(fake.writes))
                self.assertFalse(fake.requests)

    def test_coupled_preset_does_not_send_transparency_after_an_intervening_clinical_change(self):
        with fixture() as (fake, client):
            def change_mode(selected, count):
                if selected == hearing.TRANSPARENCY and count == 3:
                    age, revision, packet = fake.reports[3]
                    fake.reports[3] = (age, revision + 1, packet[:7] + b'\x01' + packet[8:])
            fake.after_read = change_mode
            with self.assertRaisesRegex(Unconfirmed, 'modes changed during'):
                client.set('accommodations-preset', 'brightness-strong')
            self.assertEqual(1, len(fake.requests))
            self.assertFalse(fake.writes)

    def test_lost_or_stale_att_readback_never_replays_the_record_write(self):
        for failure in ('stale', 'disconnect', 'receipt'):
            with self.subTest(failure=failure), fixture() as (fake, client):
                if failure == 'stale': fake.confirm_att = False
                elif failure == 'disconnect': fake.after_write = lambda: setattr(fake, 'current', False)
                else:
                    def fail(): raise OSError('Receipt lost')
                    fake.after_write = fail
                with self.assertRaises(Unconfirmed): client.set('hearing-tone', .5)
                self.assertEqual(1, len(fake.writes))

    def test_delayed_fitting_readback_and_normalized_prefix_do_not_repeat_the_write(self):
        with fixture() as (fake, client):
            original = fake.profiles[hearing.FITTING]
            requested = hearing.patch_record(original, 'tone', .5, True)
            normalized = b'\x02\x02\x60\0' + requested[4:]
            fake.read_queue[hearing.FITTING] = [original, original, original, normalized]
            self.assertTrue(client.set('hearing-tone', .5))
            self.assertEqual([(hearing.FITTING, requested)], fake.writes)

    def test_mode_confirmation_rejects_a_changed_fitting_without_silently_restoring_it(self):
        with fixture() as (fake, client):
            original = fake.profiles[hearing.FITTING]
            fake.after_request = lambda: fake.profiles.update({hearing.FITTING: changed(original, 1, 80)})
            with self.assertRaisesRegex(Unconfirmed, 'fitting changed'): client.set('hearing-aid', True)
            self.assertEqual(3, len(fake.requests))
            self.assertFalse(fake.writes)

    def test_cached_confirmation_requires_a_new_revision_and_never_replays_a_mode_command(self):
        with fixture() as (fake, client):
            fake.advance_revision = False
            with self.assertRaises(Unconfirmed): client.set('hearing-aid', True)
            self.assertEqual(3, len(fake.requests))
            self.assertEqual([0x33, 0x2c, 0x0d], [packet[6] for packet in fake.requests])
            self.assertFalse(fake.writes)

    def test_a_newer_mismatching_cached_report_is_not_confirmation(self):
        with fixture() as (fake, client):
            def different(slot, value):
                return (value[0], value[1], value[2][:-1] + b'\x01') if slot == 7 else value
            fake.report_hook = different
            with self.assertRaises(Unconfirmed): client.set('eq-low', 42)
            self.assertEqual(1, len(fake.requests))

    def test_eq_uses_its_own_cache_and_preserves_other_bands(self):
        with fixture() as (fake, client):
            self.assertTrue(client.set('eq-low', 42))
            self.assertEqual(hearing.EQ_HEADER + bytes((1, 42, 50, 70)), fake.requests[0])
            self.assertEqual(42, client.get('eq-low'))
            self.assertTrue(client.set('eq', True))
            self.assertEqual(hearing.EQ_HEADER + bytes((2, 42, 50, 70)), fake.requests[-1])
            self.assertFalse(client.set('eq', True))
            self.assertFalse(fake.writes)

    def test_aging_the_same_eq_record_is_not_an_external_edit(self):
        with fixture() as (fake, client):
            fake.cache_hook = lambda slot, count, value: (count * 10, value[1], value[2]) if slot == 7 else value
            self.assertTrue(client.set('eq-low', 42))
            self.assertEqual(1, len(fake.requests))

    def test_missing_or_malformed_eq_stays_unavailable(self):
        for packet in (None, b'bad', hearing.EQ_HEADER + bytes((0, 30, 50, 70)), hearing.EQ_HEADER + bytes((1, 101, 50, 70))):
            with self.subTest(packet=packet), fixture() as (fake, client):
                fake.reports[7] = None if packet is None else (0, 1, packet)
                with self.assertRaises(Unsupported): client.set('eq', True)
                self.assertFalse(fake.requests)

    def test_accommodations_flags_preserve_all_curve_bytes_and_do_not_enable_the_clinical_gate(self):
        with fixture() as (fake, client):
            original = fake.reports[4][2]
            self.assertTrue(client.set('headphone-accommodations', True))
            self.assertEqual(original[:9] + b'\x02\x01\x01' + original[12:], fake.requests[-1])
            self.assertEqual(2, fake.reports[3][2][7])
            self.assertFalse(fake.writes)
            self.assertFalse(client.set('headphone-accommodations', True))

    def test_legacy_pme_wire_enrollment_does_not_overwrite_device_report_semantics(self):
        with fixture() as (fake, client):
            fake.reports.update(records(pme_enrollment=0))
            self.assertTrue(client.set('accommodations-phone', True))
            self.assertEqual(2, fake.requests[0][9])
            self.assertEqual(0, fake.reports[4][2][9])
            self.assertTrue(client.get('accommodations-phone'))

    def test_preset_rechecks_clinical_and_transparency_baselines_before_any_write(self):
        for invalidation in ('clinical', 'transparency'):
            with self.subTest(invalidation=invalidation), fixture() as (fake, client):
                if invalidation == 'clinical':
                    fake.cache_hook = lambda slot, count, value: (value[0], value[1] + 1, value[2]) if slot == 2 and count > 1 else value
                else:
                    original = fake.profiles[hearing.TRANSPARENCY]
                    fake.read_queue[hearing.TRANSPARENCY] = [original, changed(original, 0, 1)]
                with self.assertRaises(RuntimeError) as result: client.set('accommodations-preset', 'brightness-strong')
                self.assertNotIsInstance(result.exception, Unconfirmed)
                self.assertFalse(fake.writes)
                self.assertFalse(fake.requests)

    def test_partial_preset_failure_is_uncertain_and_never_replays_or_restores(self):
        with fixture() as (fake, client):
            fitting = fake.profiles[hearing.FITTING]
            def fail(): raise OSError('PME receipt lost')
            fake.after_request = fail
            with self.assertRaises(Unconfirmed): client.set('accommodations-preset', 'brightness-strong')
            self.assertEqual(1, len(fake.requests))
            self.assertFalse(fake.writes)
            self.assertEqual(fitting, fake.profiles[hearing.FITTING])

    def test_stock_and_audiogram_presets_keep_the_fitting_and_couple_only_transparency_curves(self):
        for preset in ('brightness-strong', 'audiogram'):
            with self.subTest(preset=preset), fixture() as (fake, client):
                fitting, transparency = fake.profiles[hearing.FITTING], fake.profiles[hearing.TRANSPARENCY]
                self.assertTrue(client.set('accommodations-preset', preset))
                curves = hearing.STOCK_CURVES[-1] if preset == 'brightness-strong' else hearing.audiogram_curves(fitting)
                self.assertEqual(curves, fake.reports[4][2][12:])
                self.assertEqual(fitting, fake.profiles[hearing.FITTING])
                expected = bytearray(transparency)
                expected[4:36], expected[52:84] = curves[:32], curves[64:96]
                self.assertEqual(bytes(expected), fake.profiles[hearing.TRANSPARENCY])
                self.assertEqual(1, len(fake.writes))
                self.assertEqual(1, len(fake.requests))
                self.assertEqual(preset, client.get('accommodations-preset'))
                self.assertFalse(client.set('accommodations-preset', preset))


if __name__ == '__main__':
    unittest.main()
