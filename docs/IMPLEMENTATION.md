# Reviewed differences

The baseline is han-um's two-way fork. USBPods upstream is kept as a separate
Git remote for comparison. `SOURCES.json` records both revisions and the NTPods
reference file hashes used for these changes.

| Area | Baseline problem | Current behavior |
| --- | --- | --- |
| Playback start | Audio begins before both ears have settled | Encode 100 ms of silence on each real AVDTP Start and retain source PCM in a packed 32 KiB pool |
| AAC-ELD | Encoder requests 265 kbps while negotiation caps 256 kbps | Validate capabilities and select matching CBR at or below the peer's ceiling and 256 kbps |
| Unused LDAC path | Encoder output has no capacity argument and its configured packet size exceeds the transport limit | Remove LDAC selection and encoding; keep AAC-ELD, AAC-LC and SBC |
| Media packets | Unchecked lengths and clearing buffered data on failed sends | Bound capability/MTU/packet sizes; retain failed payloads and their timestamps |
| Recovery | Encoder resets or playback resumes without confirmed remote state | Confirm Suspend before resetting; close on rejected or timed-out recovery |
| Connection ownership | Late events can replace the selected peer or revive a cancelled connection | Validate address, CID and pending intent; ignore stale AVRCP and AVDTP events |
| Saved slots | Key iteration order can substitute another paired device | Require the selected saved address and its matching bond |
| Media flushing | Expired controller media can stall control or trigger disconnect | Mark only the media CID flushable, use 200 ms when non-flushable control is supported, and keep flush events from tearing down the ACL |
| Microphone lifecycle | Queued Start survives USB close; endless restart attempts | Check live USB demand, cancel queued Start, bound retries and wait for uplink quiet after Stop |
| Microphone parsing | A valid prefix can be decoded before a malformed tail | Validate the whole SDU before any decode; reject zero-length/truncated AUs |
| Decoder | Partially configured handle can become visible to callbacks | Publish the configured handle atomically; validate mono/480-sample AAC-ELD output and reset after interrupted input |
| Codec allocation | Bluetooth encoder callbacks can interrupt decoder allocation and deadlock the heap mutex | Allocate the decoder in thread context while holding the Bluetooth async lock; USB servicing remains active |
| PCM queues | Racy cursor resets and repeated queue allocations | Use bounded storage, consumer-owned microphone reset and acquire/release publication |
| USB controls | Invalid channel/format requests and repeated mute alter state | Reject unsupported requests; apply idempotent mute to PCM and preserve volume |
| Stem controls | Play/Pause mutes the dongle instead of controlling the player | Send explicit USB Consumer Control commands, with bounded queues and ordered key releases |
| BLE | BLE is linked but no scan lifecycle or receiver is implemented | Run a bounded passive receiver alongside stereo playback; pause during Classic inquiry, microphone demand/drain and USB inactivity |
| Build | Missing SDK patches can still produce a crashing or incompatible binary | Pin revisions and verify complete patched-file hashes before compilation |

## Protocol choices

The existing 48 kHz playback sample-counter RTP clock and encoder SBR setting are
preserved. A Mac capture's different clock or framing is not enough to establish
safe start, stop and recovery behavior on this radio.

The microphone ASC is `F8 E6 30 00`: AAC-ELD, mono, a signalled 48 kHz coding rate
and 480 samples per frame. USB still presents the inherited 64 kHz clock.
Calibrated duration/pitch and long-run drift checks must resolve that distinction
before changing the rate or claiming calibrated measurements.

The USB configuration contains playback, microphone, CDC diagnostics and HID
consumer controls. Its PID bitmap changes when the HID interface is added so
Windows does not reuse a cached descriptor layout from the original fork.
The prototype retains the inherited development USB VID; Apple identity is an
SDP Device ID record on Bluetooth, not a USB vendor-ID change.

Classic and BLE share the CYW43 controller and its async-context lock. Scanning
starts only after the controller reports Working and LE support, and after USB
policy permits it. Scan start/stop calls express intent; controller acceptance
and reception cadence still need a capture. Repeated policy updates do not queue
repeated scan commands.

The BLE receiver validates the complete legacy event, AD structures and Apple
type/length records before caching any prefix. It retains at most eight reports
with exact address/type, RSSI validity and monotonic reception time. Address
rotation creates a separate entry; neither model fields nor signal strength
establish ownership. Type `0x12` is retained as an opaque offline record, with no
Find My key derivation or assumption that it belongs to the paired AirPods.

## Validation limits

Native tests exercise production parsers, lifecycle code, queues and decoder
handling using small transport/codec stubs. AddressSanitizer and
UndefinedBehaviorSanitizer cover those host executions. Cross-compilation checks
the pinned Pico SDK APIs, descriptors and link limits.

USB descriptors still need Windows enumeration tests. Radio retries, negotiated
MTU, controller flush behavior, audio quality, heap high-water mark, overclock
stability and microphone clock/gain require the board. Older models and HFP
compatibility are unverified. Classic audio with concurrent BLE reception and
microphone pause/resume also needs hardware validation. No hearing profiles, calibration values or device
settings are written by the offline tests.
