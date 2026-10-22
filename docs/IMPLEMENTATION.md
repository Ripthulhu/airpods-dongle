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
| Playback pacing | Short SBC frames lose timer debt; small packets cannot drain it | Retain bounded scheduling debt, refill after successful sends and limit SBC packets to 15 frames; AAC-LC uses one access unit per packet |
| Idle playback | Encoding and radio transmission continue without an input | Request Suspend after one second idle; await confirmation, restart on demand and re-prime AAC-ELD while retaining new USB PCM |
| Recovery | Encoder resets or playback resumes without confirmed remote state | Confirm Suspend before resetting; close on rejected or timed-out recovery |
| Connection ownership | Late events can replace the selected peer or revive a cancelled connection | Validate address, CID and pending intent; ignore stale AVRCP and AVDTP events |
| Saved slots | Key iteration order can substitute another paired device | Require the selected saved address and its matching bond |
| Slot storage | Two custom buffers erase a sector on each slot/address update | Reuse the SDK's two-bank TLV journal, import the old page, skip unchanged writes and verify readback |
| Media flushing | Expired controller media can stall control or trigger disconnect | Mark only the media CID flushable, use 200 ms when non-flushable control is supported, and keep flush events from tearing down the ACL |
| Microphone lifecycle | Queued Start survives USB close; endless restart attempts | Check live USB demand, cancel queued Start, bound retries and wait for uplink quiet after Stop |
| Microphone parsing | A valid prefix can be decoded before a malformed tail | Validate the whole SDU before any decode; reject zero-length/truncated AUs |
| Decoder | Codec work blocks the Bluetooth callback | Copy bounded AUs to core 1, acknowledge resets before Start and validate completed mono/480-sample output |
| Codec allocation | Bluetooth encoder callbacks can interrupt decoder allocation and deadlock the heap mutex | Allocate the decoder in thread context while holding the Bluetooth async lock; USB servicing remains active |
| Codec memory | Generic eight-channel workspaces consume nearly the entire heap | Use a guarded stereo-encoder/mono-microphone profile; validate the linked firmware's budget on every build |
| Microphone initialization | One failed allocation prevents later recording opens from retrying | Make one attempt per request; detach failed workers before a fresh open can retry |
| PCM queues | Racy cursor resets and repeated queue allocations | Use bounded storage, generation-tagged microphone frames and acquire/release publication |
| USB controls | Invalid channel/format requests and repeated mute alter state | Reject unsupported requests; apply idempotent mute to PCM and preserve volume |
| Stem controls | Play/Pause mutes the dongle instead of controlling the player | Send explicit USB Consumer Control commands, with bounded queues and ordered key releases |
| Phone bridge | No A2DP receiver or independent phone ownership | Negotiate 48 kHz classic aptX/SBC, decode into bounded PCM and route AVRCP by peer |
| Phone startup | Input could accumulate behind AirPods priming | Open and prime the AirPods output before accepting phone Start; use one ELD frame per phone-output packet |
| Phone memory | A retained idle microphone decoder competes with the phone decoder | Let core 1 detach it, then free on core 0; close the phone codec before USB microphone takeover |
| Phone CPU use | Phone decode and AirPods encode share core 0 while core 1 waits | Decode complete phone packets on core 1; publish bounded, generation-checked PCM on core 0 |
| NTPods controls | Host-specific Bluetooth drivers are required for app controls | Use standard USB HID for pairing, reconnect, reported batteries and ANC/transparency |
| Hearing and diagnostics | USB controls cannot carry full settings records | Stage bounded HID fragments, commit once, use ATT on the existing Classic link and validate complete profile readback in NTPods |
| BLE | BLE is linked but no scan lifecycle or receiver is implemented | Run a bounded passive receiver alongside stereo playback; pause during Classic inquiry, microphone demand/drain and USB inactivity without a phone connection |
| Build | Missing SDK patches can still produce a crashing or incompatible binary | Pin revisions and verify complete patched-file hashes before compilation |

## Hardware use

Measured with GCC 13.2.1 and SDK 2.1.1 Release builds:

| Linked layout | 0.1.8 |
| --- | ---: |
| Main-heap capacity before codec allocations | 386,792 bytes |
| Flash image | 1,644,696 bytes |
| Full BLE cache copy for aggregate logs | 28 bytes |

The encoder's 4 KiB PCM buffer fills scratch-X. Compressed microphone input and
decode PCM use 3 KiB of scratch-Y. Core 1 has an explicit 16 KiB stack in ordinary
RAM; a hash-checked SDK linker derivative reserves another 16 KiB at the top of
ordinary RAM for core 0, outside the heap. Stack guards are enabled on both cores.
The lower heap capacity accounts for real stack reservations rather than assuming
the SDK's small default stacks are sufficient. Allocator locking stays enabled.

The inherited FDK decoder reserved 35,392 bytes in its internal decode function
for a coupling-element parser, even on the mono ELD path. That parser now lives
in a separate non-inlined function; ELD rejects coupling elements. The internal
function's ARM static frame is 840 bytes. Other supported codec paths retain the
original coupling parser. ARM emulation measured about 7.5 KiB of nested decoder
stack writes and 7.4 KiB for stereo ELD encoding, including the probe's margin.
These measurements support the 16 KiB reservations but do not establish hardware
stack high-water marks under USB/Bluetooth interrupts.

The target-specific `FDK_AIRPODS_PROFILE` keeps stereo AAC/AAC-ELD encoding and
raw mono ELD decoding at 48 kHz, 480 samples, with ASC `F8 E6 30 00`. Unsupported
channel counts, transports and configurations are rejected before using smaller
workspaces. The limiter remains available; non-profile FDK builds keep
their original dimensions.

| Allocation change | Bytes saved |
| --- | ---: |
| Encoder scratch: eight channels to two | 57,312 |
| Limiter capacity: eight channels/96 kHz to mono/48 kHz | 46,080 |
| Raw transport: 32 KiB to 512 bytes, with a 255-byte input limit | 32,256 |
| Decoder time workspace: 64 KiB to 8 KiB | 57,344 |
| Total | 192,992 |

The real-codec probe keeps stereo ELD at 48 kHz/256 kbps and the microphone
decoder open together. Live allocator use is 186,872 bytes, down from 379,864.
The tested allocator peak is 188,040 bytes, leaving 198,752 bytes before other
application allocations. Heap alignment and allocation order affect arena use.
Every firmware link verifies its exact codec archive fingerprint, derives capacity
from the ELF and reserves 64 KiB for other runtime use. The reserve is a build
policy, not a measured bound on the application. Tests cover malformed input,
limiter operation, playback recreation with the microphone retained and bit-exact
encoding across ten AAC/AAC-ELD configurations and five ELD VBR frame ceilings. See
[`tools/codec_probe`](../tools/codec_probe/README.md) for reproduction and limits.

Unused Bluetooth HFP, HID and network profile pools are disabled. The
implemented audio/control profiles, two HCI connections, GATT client capacity
and security tables remain allocated. HFP is not implemented by this firmware.

Playback and microphone queues use bounded contiguous copies instead of
per-sample division/copy loops. CDC drains directly into TinyUSB and retains
bytes on partial writes. Idle CDC/HID avoid up to 4,000 endpoint-state queries
per second at the existing USB service rate. Advertisement type queries derive
their result from retained raw data rather than a duplicate bitmap.

`tests/playback_benchmark.c` provides a manual host PCM benchmark. Its output
checksums match the old implementation; host timings do not measure Pico codec
load or acoustic latency. Actual peak heap usage, IRQ duration and codec deadlines
still require the board.

## Core ownership

Both Cortex-M33 cores run: core 0 retains Bluetooth, USB, playback encoding,
PCM publication and control state. RP2350's Hazard3 RISC-V cores are alternative
processors selected at boot; only two cores can run at once.
The hearing/diagnostic transport uses bounded static storage and one additional
L2CAP channel. It uses the existing ACL and allocates no heap;
fragment assembly, ATT and cached AACP reports stay on core 0.
It allocates and configures the microphone decoder in thread context under the
Bluetooth async lock, then publishes the handle. Core 1 owns phone aptX/SBC decode
or microphone decode, codec reset and decoder statistics. These inputs are
mutually exclusive. It never calls Bluetooth, USB or logging APIs.

USB and the bonded phone's encrypted RFCOMM control service have independent
receipts and bounded request buffers. They share one exclusive ATT/AACP request
owner; a competing client cannot cancel or inspect its transaction. Settings,
codec/Game Mode policy and finder work stay on core 0, without per-packet heap
allocation. The selected-pair finder borrows the eight-entry BLE cache and
resolves private addresses with an AACP-derived IRK retained only in RAM.

Eight copied compressed frames and sixteen tagged PCM frames bound microphone worker memory.
Stop, reset, disconnect and queue saturation invalidate the generation. The USB
consumer discards stale output, including an in-flight decode completed after
cancellation. Start waits for reset acknowledgement; a stopped worker cannot feed
the valid-audio watchdog merely by accepting queued input. Preparation times out
after one second, and the existing valid-frame watchdog bounds capture recovery.

Before phone codec preparation, core 0 requests release of an idle microphone
decoder and waits for core 1's acknowledgement. Core 1 discards compressed work
and atomically detaches the handle after any in-flight decode. Core 0 then closes
it. No core 1 allocation/free occurs. USB microphone takeover waits for the phone
worker to detach, then frees its codec and queue before initializing a fresh
microphone decoder. Stale output remains generation-tagged. The aptX context adds
4,404 bytes while selected, and SBC uses 1,636 bytes of static decoder state/workspace.
The phone-only worker allocation is 9,772 bytes: two bounded compressed packet
slots and one complete-packet PCM mailbox. Malformed partial output is discarded.

The diagnostics ring is 2 KiB instead of 8 KiB. Overflow retains a dropped-byte
count, and each USB service tick still drains at most 64 bytes. The saved 6 KiB
is available to the codec heap rather than reserved for old log messages.

Both cores register for SDK flash lockout before pairing/storage can write flash.
The worker uses atomic queues and wake events, leaving the multicore FIFO to the
SDK. BOOTSEL sampling also disconnects QSPI briefly, so it runs in thread context
under flash lockout instead of directly in a timer interrupt. Logs cross through
a coherent mailbox and are formatted only on core 0.

## Protocol choices

The existing 48 kHz playback sample-counter RTP clock and encoder SBR setting are
preserved. A Mac capture's different clock or framing is not enough to establish
safe start, stop and recovery behavior on this radio.

Playback sends retain at least 40 ms of bounded timer debt, or four codec frames
when larger. Successful sends can drain another packet from that debt without
advancing the clock again. Failed sends retry from the main loop, preserving the
original 200 ms recovery deadline instead of recursively requesting callbacks.

One second without USB playback, phone preparation/playback or HQ microphone
demand requests AVDTP Suspend. Only its acknowledgement marks playback stopped.
Fresh input restarts this locally suspended stream; AAC-ELD resets its encoder
and sends 100 ms of priming before source PCM. Start/Suspend have three-second
deadlines, and pending transitions block in-case self-checks. An unsolicited peer
suspension is not automatically reclaimed. Board checks must verify restart
timing, radio savings and diagnostics after idle.

The microphone ASC is `F8 E6 30 00`: AAC-ELD, mono, a signalled 48 kHz coding rate
and 480 samples per frame. USB still presents the inherited 64 kHz clock.
Calibrated duration/pitch and long-run drift checks must resolve that distinction
before changing the rate or claiming calibrated measurements.

The phone sink advertises only 48 kHz stereo classic aptX and SBC. Discovery
prefers aptX; a phone may initiate either supported configuration. Raw aptX
codewords and SBC's RTP framing have separate bounded parsers. SBC validates the
entire packet before decoding a valid prefix. Phone Start waits for successful
AirPods silence transmission, with a three-second deadline. See
[phone bridge](PHONE-TRANSPORT.md) for buffering, pairing and hardware checks.

The USB configuration contains playback, microphone, CDC diagnostics and HID
consumer controls. Its PID bitmap changes when the HID interface is added so
Windows does not reuse a cached descriptor layout from the original fork.
The prototype retains the inherited development USB VID; Apple identity is an
SDP Device ID record on Bluetooth, not a USB vendor-ID change.

Classic and BLE share the CYW43 controller and its async-context lock. Scanning
starts only after the controller reports Working and LE support, and after USB
or phone connection policy permits it. Scan start/stop calls express intent; controller acceptance
and reception cadence still need a capture. Repeated policy updates do not queue
repeated scan commands.

The BLE receiver validates the complete legacy event, AD structures and Apple
type/length records before caching any prefix. It retains at most eight reports
with exact address/type, RSSI validity and monotonic reception time. Address
rotation creates a separate entry; neither model fields nor signal strength
establish ownership. The finder separately requires the selected pair's complete
IRK response, successful private-address resolution and a valid proximity record
before using RSSI. Type `0x12` is retained as an opaque offline record, with no
Find My key derivation or assumption that it belongs to the paired AirPods.

## Validation limits

Native tests exercise production parsers, lifecycle code, queues and decoder
handling using small transport/codec stubs. AddressSanitizer and
UndefinedBehaviorSanitizer cover those host executions. Cross-compilation checks
the pinned Pico SDK APIs, descriptors and link limits.

USB descriptors still need Windows enumeration tests. Radio retries, negotiated
MTU, controller flush behavior, audio quality, heap high-water mark, overclock
stability and microphone clock/gain require the board. Older models need separate
compatibility checks. Classic audio with concurrent BLE reception and microphone
pause/resume also needs hardware validation. No hearing profiles, calibration
values or device settings are written by the offline tests.
Physical interruption during flash programming and storage-bank compaction is
also pending hardware validation; slot records use the SDK's existing journal.
