# Reviewed differences

The baseline is han-um's two-way fork. USBPods upstream is kept as a separate
Git remote for comparison. `SOURCES.json` records both revisions and the NTPods
reference file hashes used for these changes.

| Area | Baseline problem | Current behavior |
| --- | --- | --- |
| Playback start | Audio begins before both ears have settled | Encode 100 ms of silence on each real AVDTP Start; retain at most 1,440 USB PCM frames during startup, with separate active stall capacity |
| USB latency | Audio received during priming or disconnection leaves the queue almost full even when the encoder keeps pace | Keep the newest bounded USB audio, preserving whole stereo frames and independent startup priming; phone buffering is unchanged |
| Game Mode sends | A synchronous L2CAP callback can complete before the stream records its pending request | Arm the request before enqueueing and capture the delay-report revision at the actual successful send |
| AAC-ELD | Encoder requests 265 kbps while negotiation caps 256 kbps | Validate capabilities and select matching CBR at or below the peer's ceiling and 256 kbps |
| Unused LDAC path | Encoder output has no capacity argument and its configured packet size exceeds the transport limit | Remove LDAC selection and encoding; keep AAC-ELD, AAC-LC and SBC |
| Media packets | Unchecked lengths and clearing buffered data on failed sends | Bound capability/MTU/packet sizes; retain failed payloads and their timestamps |
| Playback pacing | Short SBC frames lose timer debt; small packets cannot drain it | Retain bounded scheduling debt, refill after successful sends and limit SBC packets to 15 frames; AAC-LC uses one access unit per packet |
| Idle playback | Encoding and radio transmission continue without an input | Request Suspend after one second idle; await confirmation, restart on demand and re-prime AAC-ELD while retaining new USB PCM |
| Recovery | Encoder resets or playback resumes without confirmed remote state | Confirm Suspend before resetting; close on rejected or timed-out recovery |
| Connection ownership | Late events can replace the selected peer or revive a cancelled connection | Validate address, CID and pending intent; ignore stale AVRCP and AVDTP events |
| Case return | A failed connection attempt leaves the saved headset disconnected until a manual retry | Retry the selected address with 1–8 second backoff; cancel retries during pairing and pending codec changes |
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
| USB playback | Receive callback reads before TinyUSB copies the current packet | Consume PCM after OUT is rearmed; use a monotonic 50 ms idle deadline and publish ownership only on transitions |
| Stem controls | Play/Pause mutes the dongle instead of controlling the player | Send explicit USB Consumer Control commands, with bounded queues and ordered key releases |
| Phone bridge | No A2DP receiver or independent phone ownership | Negotiate guarded 48 kHz LHDC v5 with classic aptX/SBC fallback, decode into bounded PCM and route AVRCP by peer |
| LHDC validation | Upstream decoder has approximate gain and unchecked malformed-data arithmetic | Correct the 48 kHz transform scale, bound arithmetic, check original PCM against the linked archive and require a core 1 startup timing check before advertising LHDC |
| Phone fallback | Rejected Open or repeated decoder failures can leave an unusable codec selected | Bound active Open attempts and reconnect with aptX/SBC after rejecting the failed vendor codec |
| Phone startup | Input could accumulate behind AirPods priming | Open and prime the AirPods output before accepting phone Start; use one ELD frame per phone-output packet |
| Phone memory | A retained idle microphone decoder competes with the phone decoder | Let core 1 detach it, then free on core 0; close the phone codec before USB microphone takeover |
| Phone CPU use | Phone decode and AirPods encode share core 0 while core 1 waits | Decode complete phone packets on core 1; publish bounded, generation-checked PCM on core 0 |
| NTPods controls | Host-specific Bluetooth drivers are required for app controls | Use standard USB HID for pairing, reconnect, reported batteries and ANC/transparency |
| Hearing and diagnostics | USB controls cannot carry full settings records | Stage bounded HID fragments, commit once, use ATT on the existing Classic link and validate complete profile readback in NTPods |
| BLE | BLE is linked but no scan lifecycle or receiver is implemented | Run a bounded passive receiver alongside stereo playback; pause during Classic inquiry, microphone demand/drain and USB inactivity without a phone connection |
| Build | Missing SDK patches can still produce a crashing or incompatible binary | Pin revisions and verify complete patched-file hashes before compilation |
| Radio clock | CPU overclock also raises the CYW43 SPI clock | Set a rounded-up PIO divider before radio initialization, capping SPI at 31.25 MHz |
| New phone pairing | Bond storage can overlap active audio | Require idle playback/microphone paths and cancel new pairing when they become busy; saved phones still reconnect |

## Hardware use

Measured with GCC 13.2.1 and SDK 2.3.1 Release builds:

| Linked layout | 0.1.11 |
| --- | ---: |
| Main-heap capacity before codec allocations | 385,784 bytes |
| Flash image | 1,729,304 bytes |
| Full BLE cache copy for aggregate logs | 28 bytes |

The encoder's 4 KiB PCM buffer fills scratch-X. Compressed microphone input and
decode PCM use 3 KiB of scratch-Y. Core 1 has an explicit 16 KiB stack in ordinary
RAM; hash-checked SDK linker overrides reserve another 16 KiB at the top of
ordinary RAM for core 0, outside the heap. Stack guards are enabled on both cores.
The lower heap capacity accounts for real stack reservations rather than assuming
the SDK's small default stacks are sufficient. Allocator locking stays enabled.
The selected-pair case battery fallback reuses the existing eight-entry BLE cache,
allocates no heap and stays on core 0. Shared 64-bit timestamps for BLE reception,
finder and battery observations add 120 bytes of static storage and 512 bytes
of flash against the earlier 0.1.11 link, preserving ordering and stale ages across
long absences. The main-loop poll advances this clock even without a selected
pair or while scanning is paused. The AES schedule is a temporary 176-byte stack
buffer, erased after use; core 1's codec
work and both stack reservations remain unchanged.

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
The tested allocator peak is 188,040 bytes. The historical 0.1.11 layout left
197,744 bytes before other application allocations; the 0.1.25 float-capable
layout has a 263,432-byte heap and leaves 75,392 bytes at that peak. Heap alignment
and allocation order affect arena use.
Every firmware link verifies its exact FDK/LHDC archive fingerprints, derives capacity
from the ELF and reserves 64 KiB for other runtime use. The reserve is a build
policy, not a measured bound on the application. Tests cover malformed input,
limiter operation, playback recreation with the microphone retained and bit-exact
encoding across ten AAC/AAC-ELD configurations and five ELD VBR frame ceilings. See
[`tools/codec_probe`](../tools/codec_probe/README.md) for reproduction and limits.

Unused Bluetooth HFP, HID and network profile pools are disabled. The
implemented audio/control profiles, two HCI connections, GATT client capacity
and security tables remain allocated. HFP is not implemented by this firmware.

Playback and microphone queues use bounded contiguous copies instead of
per-sample division/copy loops. Malformed USB playback packets clear the FIFO,
so a leftover tail cannot enter the next packet. USB audio notifications are
coalesced, retried on backpressure and written only by the USB service context.
CDC drains directly into TinyUSB and retains bytes on partial writes. Its RX
and TX FIFOs each hold one 64-byte packet, saving 640 bytes; endpoint buffers and
the 2 KiB diagnostic history remain separate. Idle CDC/HID avoid up to 4,000 endpoint-state queries
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
Bluetooth async lock, then publishes the handle. Core 1 owns phone LHDC/aptX/SBC decode
or microphone decode, codec reset and decoder statistics. These inputs are
mutually exclusive. It never calls Bluetooth, USB or logging APIs.

USB and the bonded phone's encrypted RFCOMM control service have independent
receipts and bounded request buffers. They share one exclusive ATT/AACP request
owner; a competing client cannot cancel or inspect its transaction. Settings,
codec/Game Mode policy and finder work stay on core 0, without per-packet heap
allocation. The selected-pair finder borrows the eight-entry BLE cache and
resolves private addresses with an AACP-derived IRK retained only in RAM.
RPC replies are rebuilt only when their contents change; transport progress and
timeouts still run on every poll. Live control-status refresh remains independent.
Hearing workflows hold a client/session lease across baseline reads, edits,
verification and mode cleanup. Typed setting writes retain their RPC owner until
sent; cancellation removes unsent work. Radio callbacks recheck the live owner,
microphone restrictions and deadlines. See [control extensions](CONTROL-EXTENSIONS.md).

Volume requests and confirmed AirPods levels are separate. The latest request is
coalesced while AVRCP is busy; accepted replies and notifications supply readback.
Requests are bound to the selected peer, and connection changes invalidate old
confirmation. Both hosts receive confirmed changes, so input takeover keeps the
current AirPods loudness instead of replaying a cached level. USB's fixed-point
scale preserves all 128 AVRCP levels on a round trip; it is a compatibility mapping,
not a calibrated acoustic gain. USB mute affects PCM and preserves that level.

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
The phone-only worker allocation is 9,780 bytes: two bounded compressed packet
slots and one complete-packet PCM mailbox. Malformed partial output is discarded.
The LHDC adapter requests 11,264 live heap bytes on ARM32 and prewarms transform
tables on core 0. No decode allocation or second decoder worker is introduced.
LHDC plus AAC-ELD and the phone worker peak at 90,816 allocator bytes in the linked
memory probe. Fixed-profile entropy histories and scratch buffers remove 4,740
bytes of retained decoder RAM before allocator rounding; decoder static state is
1,241 bytes. Compiling out disabled tracing reduces the channel decoder's ARM
stack frame from 752 to 144 bytes. Actual core 1 deadlines and full runtime memory
remain hardware checks.

The diagnostics ring is 2 KiB instead of 8 KiB. Overflow retains a dropped-byte
count, and each USB service tick still drains at most 64 bytes. The saved 6 KiB
is available to the codec heap rather than reserved for old log messages.

Both cores register for SDK flash lockout before pairing/storage can write flash.
The worker uses atomic queues and wake events, leaving the multicore FIFO to the
SDK. BOOTSEL sampling also disconnects QSPI briefly, so it runs in thread context
under flash lockout instead of directly in a timer interrupt. Logs cross through
a coherent mailbox and are formatted only on core 0.

The CYW43 SPI divider is derived from the actual system clock and rounded upward
to cap the bus at 31.25 MHz. At the inherited 250 MHz CPU clock this selects 4.0;
changing CPU speed does not implicitly overclock the radio bus. CPU overclock
stability and sustained dual-link throughput remain board checks.

The CDC console accepts only `t` as a diagnostic request, with one 64-byte receive
chunk per USB tick and a coalesced request flag. Core 0 formats the response in
thread context. It reports maximum wall time for the 500 us USB service callback
and main-loop Bluetooth lock-held section, plus the phone worker's maximum decode
time for its current allocation. Start-to-start gap maxima expose stalls between
services even when the callback itself is short. USB overruns count callbacks over 500 us, not
missing USB packets. These are wall times including higher-priority preemption;
they do not measure CPU utilization or the separate SDK Bluetooth IRQ's runtime.
Counts saturate at `UINT32_MAX`; the existing binary performance report is unchanged.

XIP hit/access counters are read and cleared together with each console snapshot.
They cover both cores and may saturate before the first request. Read once to
start a measurement window, then read again under the intended load; samples are
approximate because the hardware counters keep running during read/reset.
No cache contents or code placement change. Selective SRAM placement and bounded
HCI batching require measured contention and callback fairness first.

## Protocol choices

Boot enables saved-peer connection intent and requests Game Mode by default;
the firmware poller runs without a HID or companion client. Existing model,
firmware, codec and delay-report gates still apply. Unsupported headsets retain
normal audio. Pairing cancellation still disarms retries for the old peer.

The pinned BTstack AVRCP operation emitter declares its 19-byte scratch buffer
in the event header but passes only the bytes used to the callback. The headset
target accepts that known header or an exact header, validating actual size
against the operand count before parsing. USB PLAY and PAUSE use Consumer
Play/Pause (0xCD), so the USB host's player owns its playback state.

The existing 48 kHz playback sample-counter RTP clock and encoder SBR setting are
preserved. A Mac capture's different clock or framing is not enough to establish
safe start, stop and recovery behavior on this radio.

Audio callbacks follow the previous timer deadline, compensating for the pinned
SDK's extra millisecond in `set_timer`. A late callback retains the next tick's
phase; a missed whole interval schedules a fresh interval rather than spinning.
Live USB Game Mode also waits for completed encoder priming and 500 ms of warm
readiness, so its resampled reads cannot consume priming silence.

Playback sends retain at least 60 ms of bounded timer debt, or four codec frames
when larger. Successful sends can drain another packet from that debt without
advancing the clock again. Failed sends retry from the main loop, preserving the
original 200 ms recovery deadline instead of recursively requesting callbacks.

Live USB Game Mode changes use the captured finite 0.9/1.1 interval adjustment,
with matching stereo fractional resampling from the existing PCM ring. Each 480-frame
ELD unit consumes 432 or 528 source frames during adjustment; neither the RTP
sample counter nor encoder is reset. Pacing changes at a complete packet
boundary. Drift uses successfully sent frames against nominal elapsed time,
with a ten-second failure limit. USB uses a 960-frame steady reserve and
3,840-frame transient bound for AAC-ELD. The resampler retains float PCM, phase and a 33-tap
windowed-sinc history across blocks/rate changes, with 16 frames (0.33 ms) of
filter delay. It copies a bounded source window while excluding USB IRQs, then
filters with interrupts enabled. Quantization occurs once at the unchanged S16
encoder boundary. Two-point interpolation was rejected for continuous clock
adaptation because fractional phase attenuates high frequencies; synthetic tests
cover tone fidelity through 20 kHz.
This adapts the captured producer-clock change to USB's fixed-rate input;
it does not infer a new RTP clock or claim a reconstructed Core Audio resampler.

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

The phone sink advertises 48 kHz stereo LHDC v5 only after its startup check,
alongside classic aptX and SBC. Discovery prefers LHDC, then aptX; a phone may
initiate any available configuration. Each codec has a bounded parser, and
malformed packets cannot publish partial PCM. Phone Start waits for successful
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

Windows enumeration and actual float alt 2 have been verified on 0.1.25; current
Android and Symfonium USB-exclusive checks remain pending. Radio retries, negotiated
MTU, controller flush behavior, audio quality, heap high-water mark, overclock
stability and microphone clock/gain require the board. Older models need separate
compatibility checks. Classic audio with concurrent BLE reception and microphone
pause/resume also needs hardware validation. No hearing profiles, calibration
values or device settings are written by the offline tests.
Physical interruption during flash programming and storage-bank compaction is
also pending hardware validation; slot records use the SDK's existing journal.
