"""Leased Classic ATT settings through the dongle's existing report-3 RPC."""
import contextlib
from dataclasses import dataclass
import struct
import time
from uuid import UUID

from audio_diagnostics import HEADER, RequestRejected, Unsupported, rpc_request, typed_request


SERVICE_UUID = UUID('7798082b-b7b7-45a6-9933-563492efe04e')
LOUD_SOUND_REDUCTION_UUID = UUID('a48fec08-3921-43db-82aa-afbce8ebb4fb')
LSR_MODELS = frozenset(('A2931', 'A2699', 'A2698', 'A3047', 'A3048', 'A3049',
                        'A3063', 'A3064', 'A3065'))


class Unconfirmed(RuntimeError):
    """A single write was attempted, but its fresh device readback is unknown."""


@dataclass(frozen=True)
class Characteristic:
    handle: int
    write_opcode: int | None


class AttSettings:
    def __init__(self, device, generation=None):
        self.device = device
        status = self._status()
        self.generation = struct.unpack_from('<I', status, 8)[0]
        if generation is not None and generation != self.generation:
            raise RuntimeError('Pico session changed; read settings again')
        self.peer = bytes(status[12:18])
        if not any(self.peer) or self.peer == b'\xff' * 6:
            raise RuntimeError('No selected AirPods')
        self.token = None
        self._characteristics = None
        self.write_attempted = False

    def _status(self, admit=True):
        status = self.device.get(2)
        if len(status) != 64 or status[:7] != b'\x02' + HEADER:
            raise RuntimeError('Malformed dongle status')
        if admit and not status[7] & 2:
            raise RuntimeError('AirPods settings connection is not ready')
        if admit and status[7] & 16:
            raise RuntimeError('Stop microphone use before accessing AirPods settings')
        if admit and struct.unpack_from('<H', status, 42)[0] & 0x38 != 0x38:
            raise Unsupported('Firmware needs ATT, typed settings and workflow-lease support')
        return status

    def _owner(self, admit=True):
        status = self._status(admit)
        if (struct.unpack_from('<I', status, 8)[0] != self.generation or
                bytes(status[12:18]) != self.peer):
            raise RuntimeError('AirPods target changed; read settings again')
        if admit and self.token is not None and time.monotonic() - self.started >= 30:
            raise RuntimeError('Settings workflow expired; read settings again')

    def _lease(self, action, token):
        self._owner(action != 2)
        result = bytes(typed_request(self.device, bytes((10, action)) + struct.pack('<I', token), self.generation))
        self._owner(action != 2)
        return result

    @contextlib.contextmanager
    def workflow(self):
        """One owner-fenced lease for baseline, write and readback; never reacquire."""
        if self.token is not None:
            raise RuntimeError('A settings workflow is already active')
        self.started = self.renewed = time.monotonic()
        acquired = self._lease(0, 0)
        if len(acquired) != 5 or acquired[0] != 1 or not struct.unpack_from('<I', acquired, 1)[0]:
            raise RuntimeError('Malformed settings lease receipt')
        self.token = struct.unpack_from('<I', acquired, 1)[0]
        self._characteristics = None
        self.write_attempted = False
        failed = False
        try:
            yield self
        except BaseException:
            failed = True
            raise
        finally:
            try:
                if self._lease(2, self.token):
                    raise RuntimeError('Malformed settings lease release')
            except Exception:
                if not failed:
                    raise RuntimeError('Settings lease release was not confirmed; read again before retrying')
            finally:
                self.token = None
                self._characteristics = None

    def _request(self, payload, operation):
        if self.token is None:
            raise RuntimeError('A settings workflow lease is required')
        self._owner()
        if time.monotonic() - self.renewed >= 2:
            if self._lease(1, self.token):
                raise RuntimeError('Malformed settings lease renewal')
            self.renewed = time.monotonic()
        if operation == 2 or operation == 1 and payload[0] in (0x12, 0x52):
            self.write_attempted = True
        result = bytes(rpc_request(self.device, bytes(payload), self.generation, operation=operation))
        self._owner()
        return result

    def _exchange(self, request, opcode):
        response = self._request(request, 1)
        if response and response[0] == 1:
            if len(response) != 5 or response[1] != request[0]:
                raise RuntimeError('Malformed ATT error response')
            handle = struct.unpack_from('<H', response, 2)[0]
            start = struct.unpack_from('<H', request, 1)[0]
            end = struct.unpack_from('<H', request, 3)[0] if request[0] in (6, 8) else start
            if not start <= handle <= end:
                raise RuntimeError('ATT error belongs to a different request')
            if request[0] in (6, 8) and response[4] == 0x0a:
                return None
            raise RuntimeError(f'ATT request rejected: 0x{response[4]:02x}')
        if not response or response[0] != opcode:
            raise RuntimeError('Unexpected ATT response')
        return response

    def discover(self, uuid):
        """Discover in the settings service; no handles survive a workflow."""
        if self._characteristics is None:
            request = b'\x06\x01\x00\xff\xff\x00\x28' + SERVICE_UUID.int.to_bytes(16, 'little')
            response = self._exchange(request, 7)
            if response is None:
                raise Unsupported('AirPods settings service is unavailable')
            if len(response) != 5:
                raise RuntimeError('Invalid ATT service response')
            start, end = struct.unpack_from('<HH', response, 1)
            if not 0 < start < end:
                raise RuntimeError('Invalid ATT service range')
            found, previous_value = {}, 0
            for unused in range(16):
                request = b'\x08' + struct.pack('<HHH', start, end, 0x2803)
                page = self._exchange(request, 9)
                if page is None:
                    break
                if len(page) < 2 or page[1] not in (7, 21) or len(page) == 2 or (len(page) - 2) % page[1]:
                    raise RuntimeError('Invalid ATT characteristic response')
                each, previous = page[1], start - 1
                for at in range(2, len(page), each):
                    declaration, properties, value = struct.unpack_from('<HBH', page, at)
                    if not previous < declaration <= end or declaration <= previous_value or not declaration < value <= end:
                        raise RuntimeError('Invalid ATT characteristic range')
                    if each == 21:
                        identifier = UUID(int=int.from_bytes(page[at + 5:at + each], 'little'))
                        if identifier in found:
                            raise RuntimeError('Duplicate ATT characteristic')
                        found[identifier] = (Characteristic(value, 0x12 if properties & 8 else 0x52 if properties & 4 else None), properties)
                    previous, previous_value = declaration, value
                if previous == end:
                    break
                start = previous + 1
            else:
                raise RuntimeError('ATT discovery exceeded service limit')
            self._characteristics = found
        item = self._characteristics.get(UUID(str(uuid)))
        if item is None:
            raise Unsupported('Requested AirPods setting is unavailable')
        characteristic, properties = item
        if not properties & 2:
            raise Unsupported('AirPods setting is not readable')
        return characteristic

    def read(self, characteristic):
        self._check_characteristic(characteristic)
        response = self._exchange(b'\x0a' + struct.pack('<H', characteristic.handle), 0x0b)
        if not 2 <= len(response) < 512:
            raise RuntimeError('Incomplete ATT setting response')
        return response[1:]

    def write(self, characteristic, value):
        """Send once using advertised properties; caller must verify fresh readback."""
        self._check_characteristic(characteristic)
        if characteristic.write_opcode not in (0x12, 0x52):
            raise Unsupported('AirPods setting is not writable')
        if not isinstance(value, (bytes, bytearray)) or not 1 <= len(value) <= 509:
            raise ValueError('ATT setting must contain 1..509 bytes')
        request = bytes((characteristic.write_opcode,)) + struct.pack('<H', characteristic.handle) + value
        if characteristic.write_opcode == 0x12:
            if self._exchange(request, 0x13) != b'\x13':
                raise RuntimeError('Invalid ATT write response')
        elif self._request(request, 1):
            raise RuntimeError('Unexpected ATT Write Command response')

    def _check_characteristic(self, characteristic):
        if not any(item[0] is characteristic for item in (self._characteristics or {}).values()):
            raise RuntimeError('Discover the characteristic in the current settings workflow first')

    def cached(self, slot):
        """Read a versioned raw AACP cache slot; an absent report stays unknown."""
        if type(slot) is not int or slot not in (1, 2, 3, 4, 5, 7):
            raise ValueError('Raw AACP cache slots are 1..5 and 7')
        result = self._request(bytes((slot,)), 3)
        if not result:
            return None
        if len(result) < 9 or not struct.unpack_from('<I', result, 4)[0]:
            raise RuntimeError('Malformed cached AACP report')
        age, revision = struct.unpack_from('<II', result)
        return age, revision, result[8:]

    def request(self, packet):
        """One raw AACP send under this lease; caller owns report confirmation."""
        if not isinstance(packet, (bytes, bytearray)) or not 1 <= len(packet) <= 512:
            raise ValueError('AACP packet must contain 1..512 bytes')
        return self._request(packet, 2)

    def model(self):
        cached = self.cached(1)
        if cached is None:
            raise Unsupported('Current AirPods model report is unavailable')
        packet = cached[2]
        if not 11 <= len(packet) <= 384:
            raise RuntimeError('Malformed AirPods model report')
        if packet[:6] != b'\x04\x00\x04\x00\x1d\x00':
            raise RuntimeError('Malformed AirPods model report')
        fields, at = [], 11
        for unused in range(11):
            end = packet.find(b'\0', at)
            if end < 0:
                raise RuntimeError('Unterminated AirPods model report')
            try:
                value = packet[at:end].decode('utf-8')
            except UnicodeError as error:
                raise RuntimeError('Malformed AirPods model report') from error
            if any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in value):
                raise RuntimeError('Malformed AirPods model report')
            fields.append(value)
            at = end + 1
        return fields[1]

    def _lsr_characteristic(self):
        if self.model() not in LSR_MODELS:
            raise Unsupported('Loud Sound Reduction requires a known AirPods Pro 2 or Pro 3 model')
        return self.discover(LOUD_SOUND_REDUCTION_UUID)

    def _boolean(self, characteristic):
        value = self.read(characteristic)
        if value not in (b'\x00', b'\x01'):
            raise RuntimeError('Unsupported Loud Sound Reduction value')
        return value == b'\x01'

    def get_loud_sound_reduction(self):
        with self.workflow():
            return self._boolean(self._lsr_characteristic())

    def set_loud_sound_reduction(self, enabled):
        """Return False for a fresh no-op, True for a single confirmed write."""
        if type(enabled) is not bool:
            raise ValueError('Loud Sound Reduction expects a Boolean')
        with self.workflow():
            characteristic = self._lsr_characteristic()
            baseline = self._boolean(characteristic)
            if baseline == enabled:
                return False
            if characteristic.write_opcode is None:
                raise Unsupported('Loud Sound Reduction is read-only')
            if self._boolean(characteristic) != baseline:
                raise RuntimeError('Loud Sound Reduction changed before the write; read it again')
            written = False
            try:
                self.write(characteristic, bytes((int(enabled),)))
                written = True
                for attempt in range(5):
                    if self._boolean(characteristic) == enabled:
                        return True
                    if attempt != 4:
                        time.sleep(.075)
                raise RuntimeError('No fresh matching device readback')
            except Exception as error:
                if not self.write_attempted or not written and (isinstance(error, Unsupported) or
                        isinstance(error, RequestRejected) and error.code in (1, 2, 4)):
                    raise
                raise Unconfirmed('Loud Sound Reduction change not confirmed; it may have applied. '
                                  'Read it again before retrying: ' + str(error)) from error
