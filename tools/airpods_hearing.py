"""Named hearing/EQ edits; preserve received records and require device readback.

Layouts and mode plans mirror LibrePods core HearingModes, Accommodations and ATT types.
"""
import math
import struct
import time
import uuid

from airpods_att import AttSettings, Unconfirmed
from audio_diagnostics import Unsupported, RequestRejected


FITTING = uuid.UUID('a4120005-95c5-4d6f-9098-0f0b41457e0a')
TRANSPARENCY = uuid.UUID('d5621cc1-f7ab-45db-9403-9eaf744d5390')
HEARING_MODELS = {'A2931', 'A2699', 'A2698', 'A3047', 'A3048', 'A3049', 'A3063', 'A3064', 'A3065'}
PME_HEADER = bytes.fromhex('040004005300840002')
EQ_HEADER = bytes.fromhex('040004006300050001')
BOOL = ('on', 'off')
FIELDS = {
    'amplification': (-1.5, 1.5), 'balance': (-3.0, 3.0), 'tone': (-1.0, 1.0),
    'noise-reduction': (0.0, 1.0), 'own-voice': (0.0, 1.0), 'conversation-boost': BOOL,
}
PRESET_NAMES = tuple(f'{tuning}-{level}' for tuning in ('balanced', 'vocal', 'brightness')
                     for level in ('slight', 'moderate', 'strong'))
PARAMETERS = {
    'hearing-aid': BOOL, 'media-assist': BOOL, 'headphone-accommodations': BOOL,
    'accommodations-phone': BOOL, 'accommodations-media': BOOL,
    'accommodations-preset': PRESET_NAMES + ('audiogram',),
    'custom-transparency': BOOL, 'eq': BOOL,
    'eq-low': (0, 100), 'eq-mid': (0, 100), 'eq-high': (0, 100),
}
for prefix in ('hearing', 'transparency'):
    PARAMETERS.update({f'{prefix}-{name}': limits for name, limits in FIELDS.items()})
PARAMETERS['transparency-amplification'] = (-1.0, 2.0)
PARAMETERS['transparency-balance'] = (-3.0, 3.0)


def value_help(name):
    limits = PARAMETERS[name]
    return ', '.join(limits) if isinstance(limits[0], str) else f'number {limits[0]:g}..{limits[1]:g}'


def encode(name, value):
    limits = PARAMETERS[name]
    if isinstance(limits[0], str):
        if value not in limits:
            raise ValueError(value_help(name))
        return value == 'on' if limits == BOOL else value
    try:
        number = float(value)
    except ValueError:
        raise ValueError(value_help(name)) from None
    if not math.isfinite(number) or not limits[0] <= number <= limits[1] or (
            name.startswith('eq-') and number != int(number)):
        raise ValueError(value_help(name))
    return number


def floats(data):
    if len(data) % 4:
        raise RuntimeError('Unsupported float record length')
    values = struct.unpack('<' + 'f' * (len(data) // 4), data)
    if not all(math.isfinite(value) for value in values):
        raise RuntimeError('Non-finite hearing record')
    return values


def validate_record(data, hearing=False):
    if hearing:
        if len(data) != 104 or data[:2] not in (b'\x02\0', b'\x02\x02') or data[2:4] not in (b'\x60\0', b'\x64\0'):
            raise RuntimeError('Unsupported hearing fitting')
        values = (0.0,) + floats(data[4:])
        if any(not 0 <= values[i] <= 90 for i in (*range(1, 9), *range(13, 21))):
            raise RuntimeError('Invalid hearing fitting curves')
        amp_range = (-1.5, 1.5)
    else:
        if len(data) not in (100, 104):
            raise RuntimeError('Unsupported Custom Transparency record')
        values = floats(data)
        if values[0] not in (0, 1):
            raise RuntimeError('Invalid Custom Transparency state')
        amp_range = (-1, 2)
    if (any(not amp_range[0] <= values[i] <= amp_range[1] for i in (9, 21)) or
            any(not -1 <= values[i] <= 1 for i in (10, 22)) or
            any(values[i] not in (0, 1) for i in (11, 23)) or
            any(not 0 <= values[i] <= 1 for i in (12, 24)) or
            len(values) == 26 and not 0 <= values[25] <= 1):
        raise RuntimeError('Invalid hearing adjustments')
    return values


def record_value(data, field, hearing=False):
    values = validate_record(data, hearing)
    if field == 'custom-transparency':
        return bool(values[0])
    if field == 'amplification':
        return (values[9] + values[21]) / 2 if hearing else min(values[9], values[21])
    if field == 'balance':
        return values[21] - values[9]
    index = {'tone': 10, 'conversation-boost': 11, 'noise-reduction': 12, 'own-voice': 25}[field]
    if index >= len(values):
        raise Unsupported('Own Voice is not present in this record')
    if index != 25 and values[index] != values[index + 12]:
        return f'left={values[index]:g}, right={values[index + 12]:g}'
    return bool(values[index]) if field == 'conversation-boost' else values[index]


def patch_record(data, field, value, hearing=False):
    values = validate_record(data, hearing)
    requested = bytearray(data)
    def patch(index, number):
        if values[index] != number:
            struct.pack_into('<f', requested, index * 4, number)
    if field == 'custom-transparency':
        patch(0, int(value))
    elif field in ('amplification', 'balance'):
        amplitude = record_value(data, 'amplification', hearing) if field == 'balance' else value
        balance = values[21] - values[9] if field == 'amplification' else value
        if hearing:
            patch(9, amplitude - balance / 2)
            patch(21, amplitude + balance / 2)
        else:
            patch(9, max(-1, min(2, amplitude - min(balance, 0))))
            patch(21, max(-1, min(2, amplitude + max(balance, 0))))
    else:
        index = {'tone': 10, 'conversation-boost': 11, 'noise-reduction': 12, 'own-voice': 25}[field]
        if index >= len(values):
            raise Unsupported('Own Voice is not present in this record')
        patch(index, float(value))
        if index != 25:
            patch(index + 12, float(value))
    if hearing and requested != data:
        requested[:4] = b'\x02\x02\x64\0'
    validate_record(requested, hearing)
    return bytes(requested)


def modes(records):
    hearing, gate, pme = (record[2] for record in records)
    if (len(hearing) != 11 or hearing[:7] != bytes.fromhex('0400040009002c') or
            hearing[7] not in (1, 2) or hearing[8] not in (0, 1, 2) or any(hearing[9:]) or
            len(gate) != 11 or gate[:7] != bytes.fromhex('04000400090033') or
            gate[7] not in (1, 2) or any(gate[8:]) or
            len(pme) != 140 or pme[:9] != PME_HEADER or pme[9] not in (0, 1, 2) or
            any(flag not in (1, 2) for flag in pme[10:12])):
        raise RuntimeError('Read complete hearing-mode reports before changing settings')
    floats(pme[12:])
    if hearing[7] == 2 and hearing[8] == 1 or hearing[7] == 1 and hearing[8] == 0:
        raise RuntimeError('Hearing Aid state is unknown')
    return hearing[8] == 1, gate[7] == 1, pme[10] == 1, pme[11] == 1


def mode_plan(records, name, enabled):
    hearing, gate, voice, media = modes(records)
    h, g, pme = (record[2] for record in records)
    if enabled and (h[7] != 1 or gate != (hearing or voice or media)):
        raise RuntimeError('Hearing modes are not enrolled or not consistent')
    desired = (enabled, voice, media) if name == 'hearing-aid' else (hearing, enabled, enabled)
    packets = []
    target_gate = any(desired)
    if gate != target_gate:
        packets.append(g[:7] + bytes((1 if target_gate else 2,)) + g[8:])
    if name == 'hearing-aid' and hearing != enabled:
        packets.append(h[:8] + bytes((1 if enabled else 2,)) + h[9:])
        if enabled:
            packets.append(bytes.fromhex('0400040009000d03000000'))
    if name == 'media-assist' and (voice != enabled or media != enabled):
        packets.append(pme[:10] + bytes((1 if enabled else 2,)) * 2 + pme[12:])
    return packets, (desired[0], target_gate, desired[1], desired[2])


def audiogram_curves(fitting):
    values = validate_record(fitting, True)
    if fitting[1] != 2:
        raise RuntimeError('No writable saved audiogram')
    left, right = values[1:9], values[13:21]
    lpta = sum(left[i] for i in (1, 2, 3, 5)) / 4
    rpta = sum(right[i] for i in (1, 2, 3, 5)) / 4
    common = tuple((l + r) / 2 for l, r in zip(left, right)) if abs(lpta - rpta) < 15 else (left if lpta < rpta else right)
    normalized = tuple(common[7] if i in range(1, 7) and value == 0 else value for i, value in enumerate(common))
    return struct.pack('<8f', *normalized) * 4


class HearingSettings:
    def __init__(self, device, generation=None):
        self.att = AttSettings(device, generation)

    def _records(self):
        records = [self.att.cached(slot) for slot in (2, 3, 4)]
        if any(record is None for record in records):
            raise RuntimeError('Hearing modes have not been reported on this connection')
        modes(records)
        return records

    def _model(self):
        if self.att.model() not in HEARING_MODELS:
            raise Unsupported('Hearing settings are unavailable on this model')

    def _profile(self, hearing):
        characteristic = self.att.discover(FITTING if hearing else TRANSPARENCY)
        data = self.att.read(characteristic)
        validate_record(data, hearing)
        return characteristic, data

    def _exact_write(self, characteristic, original, requested, hearing=False, mode_guard=None):
        if requested == original:
            return False
        if self.att.read(characteristic) != original:
            raise RuntimeError('Settings changed before the write; read them again')
        if mode_guard is not None:
            mode_guard()
        self.att.write(characteristic, requested)
        for attempt in range(5):
            actual = self.att.read(characteristic)
            validate_record(actual, hearing)
            expected = actual[4:] == requested[4:] and actual[1] == 2 if hearing else actual == requested
            if expected:
                if mode_guard is not None:
                    mode_guard()
                return True
            unchanged = actual[4:] == original[4:] if hearing else actual == original
            if not unchanged or attempt == 4:
                raise Unconfirmed('Change was sent, but device readback did not match; read before retrying')
            time.sleep(.075)

    def get(self, name):
        with self.att.workflow():
            if name.startswith('eq'):
                record = self._eq()
                return record[2][9] == 2 if name == 'eq' else record[2][{'eq-low': 10, 'eq-mid': 11, 'eq-high': 12}[name]]
            self._model()
            if name == 'custom-transparency' or name.startswith(('transparency-', 'hearing-')) and name != 'hearing-aid':
                hearing = name.startswith('hearing-')
                return record_value(self._profile(hearing)[1], name.split('-', 1)[1] if name != 'custom-transparency' else name, hearing)
            records = self._records()
            hearing, gate, voice, media = modes(records)
            if name == 'hearing-aid':
                return hearing if not hearing or gate else 'unknown (hearing gate is off)'
            if name == 'media-assist':
                return (voice or media) and gate
            if name.startswith('accommodations') or name == 'headphone-accommodations':
                self._legacy(records)
                pme = records[2][2]
                if name == 'accommodations-preset':
                    for label, curves in zip(PRESET_NAMES, STOCK_CURVES):
                        if pme[12:] == curves:
                            return label
                    fitting = self._profile(True)[1]
                    return 'audiogram' if fitting[1] == 2 and pme[12:] == audiogram_curves(fitting) else 'custom'
                return (voice or media) if name == 'headphone-accommodations' else (voice if name == 'accommodations-phone' else media)
            raise ValueError('Unknown hearing setting')

    @staticmethod
    def _legacy(records):
        hearing, gate, _, _ = modes(records)
        if hearing or gate or records[2][2][9] not in (0, 2):
            raise RuntimeError('Turn Hearing Aid and Media Assist off before editing Headphone Accommodations or Custom Transparency')

    @staticmethod
    def _custom_transparency_allowed(records):
        hearing, gate, voice, media = modes(records)
        if hearing or gate or ((voice or media) and records[2][2][9] not in (0, 2)):
            raise RuntimeError('Turn Hearing Aid and Media Assist off before enabling Custom Transparency')

    def _eq(self):
        record = self.att.cached(7)
        if record is None or len(record[2]) != 13 or record[2][:9] != EQ_HEADER or record[2][9] not in (1, 2) or any(v > 100 for v in record[2][10:]):
            raise Unsupported('EQ has no valid device report; requires firmware 0.1.64 and supported AirPods')
        return record

    def _mode_guard(self, baseline, requested_pme=None):
        current = self._records()
        if (current[0][2] != baseline[0][2] or current[1][2] != baseline[1][2] or
                current[2][2] not in (baseline[2][2], requested_pme)):
            raise RuntimeError('Hearing modes changed during the edit')

    def set(self, name, value):
        attempted = False
        try:
            with self.att.workflow():
                if name.startswith('eq'):
                    before = self._eq()
                    requested = bytearray(before[2])
                    requested[9 if name == 'eq' else {'eq-low': 10, 'eq-mid': 11, 'eq-high': 12}[name]] = (2 if value else 1) if name == 'eq' else int(value)
                    if requested == before[2]:
                        return False
                    if self._eq()[1:] != before[1:]:
                        raise RuntimeError('EQ changed before the write')
                    self.att.request(bytes(requested))
                    attempted = True
                    self._confirm_cached(7, before, bytes(requested))
                    return True
                self._model()
                records = self._records()
                if name in ('hearing-aid', 'media-assist'):
                    packets, desired = mode_plan(records, name, value)
                    if not packets:
                        return False
                    fitting = None
                    if value:
                        fitting = self._profile(True)[1]
                        if fitting[1] != 2:
                            raise RuntimeError('No writable saved hearing fitting')
                        transparency = self.att.read(self.att.discover(TRANSPARENCY))
                        if transparency != b'\x02\0\0\0' and validate_record(transparency)[0] != 0:
                            raise RuntimeError('Turn Custom Transparency off first')
                    if [record[1:] for record in self._records()] != [record[1:] for record in records]:
                        raise RuntimeError('Hearing modes changed before the write')
                    for packet in packets:
                        self.att.request(packet)
                        attempted = True
                    self._confirm_modes(records, desired, packets, name)
                    if fitting is not None and self._profile(True)[1] != fitting:
                        raise Unconfirmed('Hearing fitting changed during the mode edit')
                    return True
                if name == 'custom-transparency' or name.startswith(('transparency-', 'hearing-')):
                    hearing = name.startswith('hearing-')
                    if not hearing and (name != 'custom-transparency' or value):
                        self._custom_transparency_allowed(records)
                    characteristic, original = self._profile(hearing)
                    if hearing and (original[1] != 2 or records[0][2][7] != 1):
                        raise RuntimeError('No writable enrolled hearing fitting')
                    field = name if name == 'custom-transparency' else name.split('-', 1)[1]
                    requested = patch_record(original, field, value, hearing)
                    if requested == original:
                        return False
                    if [record[1:] for record in self._records()] != [record[1:] for record in records]:
                        raise RuntimeError('Hearing modes changed before the write')
                    return self._exact_write(characteristic, original, requested, hearing,
                                             lambda: self._mode_guard(records))
                self._legacy(records)
                original = records[2][2]
                requested = bytearray(original)
                characteristic, transparency = self._profile(False)
                fitting = self._profile(True)[1]
                coupled = bytearray(transparency)
                if name == 'accommodations-preset':
                    if len(transparency) != 104:
                        raise Unsupported('Preset requires the reviewed 104-byte Custom Transparency record')
                    curves = audiogram_curves(fitting) if value == 'audiogram' else STOCK_CURVES[PRESET_NAMES.index(value)]
                    requested[12:] = curves
                    coupled[4:36], coupled[52:84] = curves[:32], curves[64:96]
                    if value == 'audiogram':
                        for index in (*range(1, 9), *range(13, 21)):
                            number = struct.unpack_from('<f', coupled, index * 4)[0]
                            struct.pack_into('<f', coupled, index * 4, max(0, min(80, number)))
                elif name == 'headphone-accommodations':
                    requested[10:12] = bytes((1 if value else 2,)) * 2
                else:
                    requested[10 if name == 'accommodations-phone' else 11] = 1 if value else 2
                if requested == original and coupled == transparency:
                    return False
                if [record[1:] for record in self._records()] != [record[1:] for record in records]:
                    raise RuntimeError('Hearing modes changed before the write')
                if self._profile(True)[1] != fitting or self.att.read(characteristic) != transparency:
                    raise RuntimeError('Hearing records changed before the write')
                if requested != original:
                    wire = bytearray(requested)
                    wire[9] = 2
                    self.att.request(bytes(wire))
                    attempted = True
                if coupled != transparency:
                    attempted = self._exact_write(characteristic, transparency, bytes(coupled),
                        mode_guard=lambda: self._mode_guard(records, bytes(requested))) or attempted
                if requested != original:
                    self._confirm_cached(4, records[2], bytes(requested))
                current = self._records()
                self._legacy(current)
                if (current[0][2] != records[0][2] or current[1][2] != records[1][2] or
                        current[2][2] != requested or self._profile(True)[1] != fitting or
                        self.att.read(characteristic) != coupled):
                    raise Unconfirmed('Unrelated hearing settings changed during the edit')
                return True
        except Unconfirmed:
            raise
        except (OSError, RuntimeError) as error:
            if not attempted and (isinstance(error, Unsupported) or
                    isinstance(error, RequestRejected) and error.code in (1, 2, 4)):
                raise
            if self.att.write_attempted:
                raise Unconfirmed(f'Change may have applied: {error}. Read settings before retrying.') from error
            raise

    def _confirm_cached(self, slot, before, expected):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            current = self.att.cached(slot)
            if current is not None and current[1] != before[1]:
                if current[2] == expected:
                    return
                raise Unconfirmed('A newer device report differs from the requested setting')
            time.sleep(.1)
        raise Unconfirmed('No fresh matching device report; read settings before retrying')

    def _confirm_modes(self, before, expected, packets, name):
        changed_slots = {2 if p[4] == 9 and p[6] == 0x2c else 3 if p[4] == 9 and p[6] == 0x33 else 4
                         for p in packets if not (p[4] == 9 and p[6] == 0x0d)}
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            current = self._records()
            if modes(current) == expected and all(current[slot - 2][1] != before[slot - 2][1] for slot in changed_slots):
                previous, reported = before[2][2], current[2][2]
                inactive_ha_curves = name == 'hearing-aid' and not expected[2] and not expected[3] and (
                    not any(previous[12:]) or not any(reported[12:]))
                if (current[0][2][7] == before[0][2][7] and reported[8:10] == previous[8:10] and
                        (reported[12:] == previous[12:] or inactive_ha_curves)):
                    return
                raise Unconfirmed('Unrelated hearing configuration changed')
            time.sleep(.1)
        raise Unconfirmed('Hearing mode did not receive fresh matching reports; read before retrying')


# Anonymous captured stock curves from LibrePods core Accommodations.kt, tuning then level.
STOCK_CURVES = tuple(bytes.fromhex(value) for value in (
    "08ac8e41d9ce8f411b2f8f41ba499c41ba49a841b6f3d041c976f641c976f64108ac8e41d9ce8f411b2f8f41ba499c41ba49a841b6f3d041c976f641c976f64108ac8e41d9ce8f411b2f8f41ba499c41ba49a841b6f3d041c976f641c976f64108ac8e41d9ce8f411b2f8f41ba499c41ba49a841b6f3d041c976f641c976f641",
    "6210da413d0af04160e50c42ac9c1e421b2f29429e6f334293184642931846426210da413d0af04160e50c42ac9c1e421b2f29429e6f334293184642931846426210da413d0af04160e50c42ac9c1e421b2f29429e6f334293184642931846426210da413d0af04160e50c42ac9c1e421b2f29429e6f33429318464293184642",
    "ac9c3b4204563e42751343422731604252b86b4208ac6b42d1226142d1226142ac9c3b4204563e42751343422731604252b86b4208ac6b42d1226142d1226142ac9c3b4204563e42751343422731604252b86b4208ac6b42d1226142d1226142ac9c3b4204563e42751343422731604252b86b4208ac6b42d1226142d1226142",
    "426013411b2f2b4139b454419eefea41b81e1e42e17a0842cff7d941cff7d941426013411b2f2b4139b454419eefea41b81e1e42e17a0842cff7d941cff7d941426013411b2f2b4139b454419eefea41b81e1e42e17a0842cff7d941cff7d941426013411b2f2b4139b454419eefea41b81e1e42e17a0842cff7d941cff7d941",
    "25063d41ba498641022be741c1ca534206816a42986e7042aec77642aec7764225063d41ba498641022be741c1ca534206816a42986e7042aec77642aec7764225063d41ba498641022be741c1ca534206816a42986e7042aec77642aec7764225063d41ba498641022be741c1ca534206816a42986e7042aec77642aec77642",
    "b81ece4139b40c42df4f5b429398864231c88c42a2858e42b21d9842b21d9842b81ece4139b40c42df4f5b429398864231c88c42a2858e42b21d9842b21d9842b81ece4139b40c42df4f5b429398864231c88c42a2858e42b21d9842b21d9842b81ece4139b40c42df4f5b429398864231c88c42a2858e42b21d9842b21d9842",
    "39b4144179e91c41d1223b4196438f410000b541b6f31c42500d5542500d554239b4144179e91c41d1223b4196438f410000b541b6f31c42500d5542500d554239b4144179e91c41d1223b4196438f410000b541b6f31c42500d5542500d554239b4144179e91c41d1223b4196438f410000b541b6f31c42500d5542500d5542",
    "17d9af4114aeba4191edd2410ad70b4210d82f425e3a6542c10a8b42c10a8b4217d9af4114aeba4191edd2410ad70b4210d82f425e3a6542c10a8b42c10a8b4217d9af4114aeba4191edd2410ad70b4210d82f425e3a6542c10a8b42c10a8b4217d9af4114aeba4191edd2410ad70b4210d82f425e3a6542c10a8b42c10a8b42",
    "10d825424ee22a421f8535425c8f4f42c9766742b8de8742b21d9442b21d944210d825424ee22a421f8535425c8f4f42c9766742b8de8742b21d9442b21d944210d825424ee22a421f8535425c8f4f42c9766742b8de8742b21d9442b21d944210d825424ee22a421f8535425c8f4f42c9766742b8de8742b21d9442b21d9442",
))
