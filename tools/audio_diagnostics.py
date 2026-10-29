#!/usr/bin/env python3
"""Read-only Pico audio diagnostics over Windows HID or Android/Linux hidraw.

No companion app is needed. Run on Android with root and a Python installation.
Output is JSON Lines; use --output to retain a bounded capture locally.
"""
import argparse
import ctypes as C
import glob
import json
import os
import struct
import sys
import time

HEADER = b'APDG' + bytes((1, 64))
USB_VENDOR = 0xcafe
USB_PRODUCTS = (0x4015, 0x4005)  # Audio composite or Bluetooth control-only mode.


class Hid:
    def __init__(self, serial=None):
        self.handle = None
        if os.name != 'nt':
            import fcntl
            self.ioctl = fcntl.ioctl
            found = []
            for path in glob.glob('/dev/hidraw*'):
                fd, keep = None, False
                try:
                    fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
                    info = bytearray(8)
                    self.ioctl(fd, 0x80084803, info, True)
                    _, vid, pid = struct.unpack('<IHH', info)
                    if vid != USB_VENDOR or pid not in USB_PRODUCTS:
                        continue
                    if serial:
                        unique = bytearray(256)
                        self.ioctl(fd, 0x81004808, unique, True)  # HIDIOCGRAWUNIQ(256)
                        if unique.split(b'\0', 1)[0].decode('utf-8', 'replace') != serial:
                            continue
                    found.append(fd)
                    keep = True
                except OSError:
                    continue
                finally:
                    if fd is not None and not keep:
                        os.close(fd)
            if len(found) != 1:
                for fd in found:
                    os.close(fd)
                raise RuntimeError('Expected exactly one matching Pico HID interface (CAFE:4015/4005)')
            self.handle = found[0]
            return
        from ctypes import wintypes as W
        self.kernel = C.WinDLL('kernel32', use_last_error=True)
        self.hid = C.WinDLL('hid', use_last_error=True)
        setup = C.WinDLL('setupapi', use_last_error=True)

        class Guid(C.Structure):
            _fields_ = [('a', W.DWORD), ('b', W.WORD), ('c', W.WORD), ('d', C.c_ubyte * 8)]

        class Interface(C.Structure):
            _fields_ = [('size', W.DWORD), ('guid', Guid), ('flags', W.DWORD), ('reserved', C.c_void_p)]

        class Attributes(C.Structure):
            _fields_ = [('size', W.DWORD), ('vid', W.WORD), ('pid', W.WORD), ('version', W.WORD)]

        class Overlapped(C.Structure):
            _fields_ = [('internal', C.c_size_t), ('internal_high', C.c_size_t),
                        ('offset', W.DWORD), ('offset_high', W.DWORD), ('event', W.HANDLE)]

        self.Overlapped = Overlapped
        self.kernel.CreateEventW.restype = W.HANDLE
        self.kernel.CreateEventW.argtypes = [C.c_void_p, W.BOOL, W.BOOL, W.LPCWSTR]
        self.kernel.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
        self.kernel.DeviceIoControl.argtypes = [W.HANDLE, W.DWORD, C.c_void_p, W.DWORD,
                                               C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(Overlapped)]
        self.kernel.CancelIoEx.argtypes = [W.HANDLE, C.POINTER(Overlapped)]
        self.kernel.GetOverlappedResult.argtypes = [W.HANDLE, C.POINTER(Overlapped), C.POINTER(W.DWORD), W.BOOL]

        setup.SetupDiGetClassDevsW.restype = W.HANDLE
        setup.SetupDiGetClassDevsW.argtypes = [C.POINTER(Guid), W.LPCWSTR, W.HWND, W.DWORD]
        setup.SetupDiEnumDeviceInterfaces.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(Guid), W.DWORD, C.POINTER(Interface)]
        setup.SetupDiGetDeviceInterfaceDetailW.argtypes = [W.HANDLE, C.POINTER(Interface), C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
        setup.SetupDiDestroyDeviceInfoList.argtypes = [W.HANDLE]
        self.kernel.CreateFileW.restype = W.HANDLE
        self.kernel.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
        self.kernel.CloseHandle.argtypes = [W.HANDLE]
        self.hid.HidD_GetAttributes.argtypes = [W.HANDLE, C.POINTER(Attributes)]
        self.hid.HidD_GetPreparsedData.argtypes = [W.HANDLE, C.POINTER(C.c_void_p)]
        self.hid.HidD_FreePreparsedData.argtypes = [C.c_void_p]
        self.hid.HidP_GetCaps.argtypes = [C.c_void_p, C.c_void_p]
        self.hid.HidD_GetSerialNumberString.argtypes = [W.HANDLE, C.c_void_p, W.DWORD]
        for name in ('HidD_GetFeature', 'HidD_SetFeature'):
            getattr(self.hid, name).argtypes = [W.HANDLE, C.c_void_p, W.DWORD]
        guid = Guid()
        self.hid.HidD_GetHidGuid(C.byref(guid))
        info = setup.SetupDiGetClassDevsW(C.byref(guid), None, None, 0x12)
        if info == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        found = []
        try:
            for index in range(256):
                interface = Interface()
                interface.size = C.sizeof(interface)
                if not setup.SetupDiEnumDeviceInterfaces(info, None, C.byref(guid), index, C.byref(interface)):
                    if C.get_last_error() == 259:
                        break
                    raise C.WinError(C.get_last_error())
                required = W.DWORD()
                setup.SetupDiGetDeviceInterfaceDetailW(info, C.byref(interface), None, 0, C.byref(required), None)
                if not 8 <= required.value <= 65536:
                    raise RuntimeError('Invalid HID interface detail size')
                detail = C.create_string_buffer(required.value)
                struct.pack_into('<I', detail, 0, 8 if C.sizeof(C.c_void_p) == 8 else 6)
                if not setup.SetupDiGetDeviceInterfaceDetailW(info, C.byref(interface), detail, required, None, None):
                    raise C.WinError(C.get_last_error())
                path = C.wstring_at(C.addressof(detail) + 4)
                if not any(f'vid_cafe&pid_{pid:04x}' in path.lower() for pid in USB_PRODUCTS):
                    continue
                handle = self.kernel.CreateFileW(path, 0xc0000000, 3, None, 3, 0x40000000, None)
                if handle == C.c_void_p(-1).value:
                    raise C.WinError(C.get_last_error())
                keep = False
                try:
                    attr = Attributes()
                    attr.size = C.sizeof(attr)
                    if (not self.hid.HidD_GetAttributes(handle, C.byref(attr)) or
                        attr.vid != USB_VENDOR or attr.pid not in USB_PRODUCTS):
                        continue
                    prep = C.c_void_p()
                    if not self.hid.HidD_GetPreparsedData(handle, C.byref(prep)):
                        raise C.WinError(C.get_last_error())
                    caps = C.create_string_buffer(64)
                    try:
                        status = self.hid.HidP_GetCaps(prep, caps)
                    finally:
                        self.hid.HidD_FreePreparsedData(prep)
                    if status != 0x110000 or struct.unpack_from('<HH', caps) != (1, 0xff31) or struct.unpack_from('<H', caps, 8)[0] != 64:
                        continue
                    value = C.create_unicode_buffer(256)
                    if not self.hid.HidD_GetSerialNumberString(handle, value, C.sizeof(value)):
                        raise C.WinError(C.get_last_error())
                    if serial and value.value != serial:
                        continue
                    found.append(handle)
                    keep = True
                finally:
                    if not keep:
                        self.kernel.CloseHandle(handle)
        finally:
            setup.SetupDiDestroyDeviceInfoList(info)
        if len(found) != 1:
            for handle in found:
                self.kernel.CloseHandle(handle)
            raise RuntimeError('Expected exactly one matching Pico vendor HID collection')
        self.handle = found[0]

    def close(self):
        if self.handle is not None:
            if os.name == 'nt':
                self.kernel.CloseHandle(self.handle)
            else:
                os.close(self.handle)
            self.handle = None

    def get(self, report):
        data = bytearray(64)
        data[0] = report
        if os.name == 'nt':
            self._feature(False, data)
        else:
            self.ioctl(self.handle, 0xC0404807, data, True)
        if data[:7] != bytes((report,)) + HEADER:
            raise RuntimeError('Unsupported HID report identity/version')
        return data

    def set(self, data):
        if os.name == 'nt':
            self._feature(True, data)
        else:
            self.ioctl(self.handle, 0xC0404806, data)

    def _feature(self, writing, data):
        from ctypes import wintypes as W
        event = self.kernel.CreateEventW(None, True, False, None)
        if not event:
            raise C.WinError(C.get_last_error())
        transfer = self.Overlapped()
        transfer.event = event
        count = W.DWORD()
        buffer = (C.c_ubyte * 64).from_buffer(data)
        try:
            ok = self.kernel.DeviceIoControl(self.handle, 0x000b0191 if writing else 0x000b0192,
                                            buffer if writing else None, 64 if writing else 0,
                                            None if writing else buffer, 0 if writing else 64,
                                            C.byref(count), C.byref(transfer))
            if not ok and C.get_last_error() != 997:
                raise C.WinError(C.get_last_error())
            if not ok and self.kernel.WaitForSingleObject(event, 400) != 0:
                self.kernel.CancelIoEx(self.handle, C.byref(transfer))
                # The kernel owns buffer/OVERLAPPED until cancellation completes.
                self.kernel.GetOverlappedResult(self.handle, C.byref(transfer), C.byref(count), True)
                raise TimeoutError('USB feature transfer exceeded 400 ms')
            if not self.kernel.GetOverlappedResult(self.handle, C.byref(transfer), C.byref(count), False):
                raise C.WinError(C.get_last_error())
            if not writing and count.value != 64:
                raise RuntimeError('Incomplete USB feature report')
        finally:
            self.kernel.CloseHandle(event)


class Unsupported(RuntimeError):
    pass


TRACE_STATS = ('usb_packets', 'usb_frames', 'usb_gap_max_us', 'usb_bad_packets',
               'usb_short_reads', 'startup_dropped_frames', 'active_dropped_frames',
               'startup_zero_frames', 'active_zero_frames', 'timer_late_max_us',
               'encode_max_us', 'send_wait_max_us', 'send_gap_max_us', 'packet_age_max_us',
               'controller_flushes', 'controller_completed_packets', 'discarded_debt_frames',
               'timeline_recoveries', 'pcm_min', 'pcm_max', 'clock_rate_q16',
               'ble_scan_requested', 'usb_alternate_setting', 'encode_unit_max_us')
TRACE_EVENTS = {1: 'drop', 2: 'zero_fill', 3: 'timer_late', 4: 'send_wait',
                5: 'timeline_recovery', 6: 'controller_flush', 7: 'usb_error',
                8: 'format', 9: 'game'}


def decode_trace(data):
    if len(data) != 512 or data[0:2] != b'\x01\x10' or data[2] > 24:
        raise RuntimeError('Unsupported audio trace layout')
    result = dict(zip(('time_us_wrapped', 'event_serial', 'pcm_used', 'sample_debt',
                       'game_phase', 'rtp_timestamp', 'reserved'), struct.unpack_from('<7I', data, 4)))
    result['stats'] = dict(zip(TRACE_STATS, struct.unpack_from('<24I', data, 32)))
    result['events'] = []
    for i in range(data[2]):
        us, kind, a, b = struct.unpack_from('<4I', data, 128 + 16*i)
        result['events'].append(dict(time_us_wrapped=us, kind=TRACE_EVENTS.get(kind, kind), a=a, b=b))
    return result


def _transaction(device, payload):
    read = len(payload) == 1 and payload[0] in (5, 9, 11, 13, 15)
    profile = len(payload) == 2 and payload[0] in (4, 12, 14) and payload[1] in (0, 1)
    if not (read or profile):
        raise ValueError('Only diagnostic reads and explicit audio profiling controls are allowed')
    generation = struct.unpack_from('<I', device.get(2), 8)[0]
    initial = device.get(3)
    if initial[21] in (1, 2):
        raise RuntimeError('Another HID transaction is busy; close companion software')
    sequence = (struct.unpack_from('<I', initial, 12)[0] + 1) & 0xffffffff or 1

    def send(op, total=0, offset=0, payload=b''):
        data = bytearray(64)
        data[:7] = bytes((3,)) + HEADER
        data[7] = op
        struct.pack_into('<IIHHB', data, 8, generation, sequence, total, offset, len(payload))
        data[22:22 + len(payload)] = payload
        device.set(data)

    def wait(phase, offset):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            data = device.get(3)
            if struct.unpack_from('<II', data, 8) == (generation, sequence):
                if data[21] == 4:
                    if data[22] == 7:
                        raise Unsupported('Firmware does not expose this diagnostic feature')
                    raise RuntimeError('RPC failure %d' % data[22])
                if data[21] == phase and struct.unpack_from('<H', data, 18)[0] == offset:
                    if data[20] > 42:
                        raise RuntimeError('Oversized HID page')
                    return data
            time.sleep(.005)
        raise TimeoutError('Pico diagnostic read timed out')

    send(7, len(payload), payload=payload)
    wait(1, len(payload))
    send(6)
    page = wait(3, 0)
    total = struct.unpack_from('<H', page, 16)[0]
    if not 0 <= total <= 4096 or (read and not total):
        raise RuntimeError('Invalid diagnostic length')
    output = bytearray()
    while total:
        if struct.unpack_from('<H', page, 16)[0] != total or not page[20]:
            raise RuntimeError('Invalid diagnostic pagination')
        output.extend(page[22:22 + page[20]])
        if len(output) > total:
            raise RuntimeError('Diagnostic page exceeds declared length')
        if len(output) == total:
            break
        send(4, offset=len(output))
        page = wait(3, len(output))
    if struct.unpack_from('<I', device.get(2), 8)[0] != generation:
        raise RuntimeError('Pico session changed during read')
    return output


def feature(device, command):
    if command not in (5, 9, 11, 13, 15):
        raise ValueError('Collector permits audio/performance/trace reads only')
    return _transaction(device, bytes((command,)))


def decode_inspect(data):
    if len(data)!=400 or data[0]!=1 or data[1]!=3 or data[2]>3 or not 1<=data[3]<=8:
        raise RuntimeError('Unsupported audio inspection layout')
    names=('usb_samples','usb_nonzero_samples','usb_peak_q15','usb_last_nonzero_us',
           'pcm_samples','pcm_nonzero_samples','pcm_peak_q15','pcm_last_nonzero_us',
           'submitted_au_count','submitted_au_us','submitted_au_bytes','encoder_generation',
           'submitted_au_sequence','usb_last_packet_us')
    result=dict(zip(names,struct.unpack_from('<14I',data,8)))
    if result['submitted_au_bytes']>2047 or bool(data[2]&1)!=(result['submitted_au_bytes']>320):
        raise RuntimeError('Invalid inspected AAC-ELD length')
    result.update(snapshot_us=struct.unpack_from('<I',data,4)[0],
                  rtp_timestamp=struct.unpack_from('<I',data,64)[0],
                  bad_eld_packets=struct.unpack_from('<I',data,76)[0],
                  truncated=bool(data[2]&1),active=bool(data[2]&2),asc_hex=data[68:68+data[3]].hex(),
                  au_hex=data[80:80+min(result['submitted_au_bytes'],320)].hex())
    return result


def decode_phone(data):
    if (len(data) not in (52, 64) or data[1] > 3 or
            (data[0], data[3], len(data)) not in ((1, 12, 52), (2, 15, 64))):
        raise RuntimeError('Unsupported phone diagnostic layout')
    names = ('packets', 'malformed', 'sequence_gaps', 'submit_failures',
             'decode_failures', 'buffer_resets', 'decoded_frames', 'last_packet_ms',
             'queued_packets', 'output_ready', 'last_decode_us', 'max_decode_us')
    if data[0] == 2:
        names += ('max_media_gap_ms', 'max_pcm_gap_ms', 'timestamp_discontinuities')
    result = dict(zip(names, struct.unpack_from('<' + str(data[3]) + 'I', data, 4)))
    if result['output_ready'] > 1:
        raise RuntimeError('Invalid phone output state')
    result.update(codec=data[1], flags=data[2])
    return result


def sample(device, trace=True, inspect=False, phone=False):
    audio, perf = feature(device, 9), feature(device, 5)
    if len(audio) != 20 or len(perf) != 64 or audio[0] != 1 or perf[0] != 1:
        raise RuntimeError('Unsupported audio/performance layout')
    names = ('uptime_ms', 'pcm_used', 'pcm_capacity', 'pcm_peak', 'underflows', 'drops',
             'encoded_units', 'encode_us', 'encode_max_us', 'send_failures', 'recoveries',
             'heap_capacity', 'heap_allocated', 'heap_remaining', 'heap_arena')
    result = dict(time=time.time(), game_requested=bool(audio[12]), game_phase=audio[13],
                  audio_flags=audio[15], delay_tenths_ms=struct.unpack_from('<H', audio, 16)[0],
                  performance=dict(zip(names, struct.unpack_from('<15I', perf, 4))))
    if trace:
        try:
            result['trace'] = decode_trace(feature(device, 11))
        except Unsupported:
            result['trace_supported'] = False
    if inspect:
        result['inspect']=decode_inspect(feature(device,13))
    if phone:
        try:
            result['phone']=decode_phone(feature(device,15))
        except Unsupported:
            result['phone_supported']=False
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--serial', help='Require this Pico serial on Windows')
    parser.add_argument('--output', help='JSONL destination (default stdout)')
    parser.add_argument('--legacy', action='store_true', help='Only existing feature 5/9 snapshots')
    parser.add_argument('--inspect', action='store_true', help='Include signal levels and the latest submitted AAC-ELD frame')
    parser.add_argument('--phone', action='store_true', help='Include phone input queue and decoder counters')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 1800:
        parser.error('--seconds must be 1..1800')
    if args.legacy and (args.inspect or args.phone):
        parser.error('--legacy cannot be combined with --inspect or --phone')
    output = open(args.output, 'x', encoding='utf-8') if args.output else sys.stdout
    device = Hid(args.serial)
    inspecting=False
    try:
        if args.inspect:
            _transaction(device,bytes((14,1)));inspecting=True
        renewed=time.monotonic()
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            started = time.monotonic()
            if inspecting and started-renewed>=10:
                _transaction(device,bytes((14,1)));renewed=started
            print(json.dumps(sample(device, not args.legacy, args.inspect, args.phone)), file=output, flush=True)
            time.sleep(max(0, 1 - (time.monotonic() - started)))
    finally:
        if inspecting:
            try:
                _transaction(device,bytes((14,0)))
            except Exception as error:
                print('Inspection stop failed; firmware expires it after 120 seconds: '+str(error),file=sys.stderr)
        device.close()
        if output is not sys.stdout:
            output.close()


if __name__ == '__main__':
    main()
