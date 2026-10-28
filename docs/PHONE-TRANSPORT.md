# Phone bridge

Firmware 0.1.26 pairs with the Pixel 10 Pro XL and plays LHDC through the Pico,
but its phone buffering can lose samples. Version 0.1.27's larger PCM reserve
did not fix the audible crackle: hardware counters exposed compressed-input
queue overflow and repeated decoder/PCM resets. Version 0.1.28 stopped those
losses in the initial capture and improved playback, but PCM overflow, crackle
and eventual silence remained. Version 0.1.29 corrected output accounting and
automatically recovered without dropping the phone connection, but the listener
still reported interruptions. Version 0.1.30's eight-slot controller window
improved playback, but the listener still heard crackle. Version 0.1.31 tests
two AAC-ELD units per phone-output packet. Send waits fell to 8 ms, but small
hitches remained. Version 0.1.32 adds input timing diagnostics and a replay-tested
90 ms jitter reserve; its hardware validation is pending.
The separate app control service remains untested on Pico hardware.

```text
Phone -- LHDC v5 / aptX / SBC --> Pico 2 W -- AAC-ELD / AAC / SBC --> AirPods
```

The phone pairs with **AirPods Dongle** as an ordinary Bluetooth audio output.
The Pico owns the AirPods bond, Apple Device ID, AACP and output codec. Android
needs no vendor-ID modification. Power the Pico externally for wireless use;
an active USB playback or recording stream takes priority over phone audio.

## Pairing and controls

Pair the AirPods first: put them in pairing mode, then hold BOOTSEL for at least
one second but less than three seconds and release. To pair the phone, hold for
three seconds and release, then select **AirPods Dongle** in the phone's Bluetooth
settings within two minutes. The phone bond is saved separately from the two
AirPods slots. Only the selected AirPods and saved phone are accepted outside
that explicit pairing window. A new phone must complete encrypted, bonded access
before its identity is retained.

Pause playback and close microphone use before pairing a new phone. Firmware
rejects a new pairing request while audio is active or changing state. Once
opened, the window survives idle output transitions, but playback/microphone
demand or the original two-minute deadline closes it. Saved phones can reconnect during playback.
An already in-flight BTstack link-key notification can still write flash before
the application's cancellation check; this does not guarantee uninterrupted
audio when a host starts playback during bonding.

AirPods stem media commands and absolute volume go to the phone during phone
playback. USB media commands stay on the USB path. Music only: phone calls, HFP,
SCO and a BLE settings interface are not implemented.

The settings service accepts legacy Call Management (`0x24`) values `00 03 00 00`
(double press to end a call) and `00 02 00 00` (single press to end). These do not
establish a complementary mute assignment or add phone-call transport. Newer
Apple versions contain independent end-call and mute fields; do not flatten a
newer or unknown report into one of these legacy values. Complete modern writes
and physical call behavior through this dongle remain unverified.

Confirmed AirPods volume is mirrored to both hosts. A host volume request remains
an intent until the AirPods confirm it; temporary AVRCP busy states retain the
latest request. A controls-only connection or input switch does not replay a
reported level as a new request. New AirPods connections start with unknown
volume until fresh readback arrives. USB mute preserves volume and silences PCM.

LibrePods connects to the saved phone's encrypted, bonded RFCOMM service for
device settings, hearing profiles, saved checks, audio profiles, performance
readings and nearby finding. See [control extensions](CONTROL-EXTENSIONS.md) for
the UUID and wire format. These controls do not need a PC, a phone vendor-ID
override or an audio stream. The microphone path still requires USB recording.
Opening settings never takes audio; no PC/phone handoff commands are added.

Automatic pause belongs to the app using the active input. It consumes fresh
ear-state reports and resumes only the player it paused, while it still owns
that pause. Disconnect, stale reports, another input or a manual playback change
revokes that ownership. Stored device ear detection is a separate AirPods setting.

## Codec and buffering

- Input is 48 kHz stereo. Discovery prefers LHDC v5, then classic aptX, then SBC;
  a phone initiating configuration can choose an available codec. LHDC is limited
  to 24-bit input, 5 ms lossy frames and 256–400 kbps. The bridge retains its existing
  16-bit PCM path. No LHDC lossless/LL, aptX LL/HD/Adaptive, AAC or LDAC input is
  advertised. Rejected vendor-codec setup or failed allocation withdraws that
  endpoint and makes one reconnect attempt with the remaining codecs.
  A rejected Open or failed media-channel setup uses the same recovery. Three
  consecutive completed LHDC decode failures also withdraw LHDC; successful PCM
  clears the count. Packet gaps, malformed RTP and input backpressure do not
  mark a codec unsupported. Malformed packets and RTP gaps clear stale work;
  input congestion preserves already queued audio and resets decoder history
  at the first accepted packet after the loss.
  Actual Open attempts have a three-second deadline. A peer that configures audio
  and waits before requesting Open remains connected.
- Before Bluetooth starts, core 1 silently decodes 32 synthetic LHDC music frames
  through the actual worker. LHDC is offered only if every frame completes in
  under 4 ms, produces 240 stereo samples, both channels produce nonzero output
  across the fixture, and the worker detaches cleanly. Missing
  timing, decode errors or timeouts leave aptX/SBC available. USB is serviced while
  waiting; no decoded audio is played. An unacknowledged worker detach aborts
  Bluetooth startup instead of freeing memory still used by core 1. This is a
  startup sample at the configured clock, not a guarantee under radio/USB load.
- Classic aptX contains raw four-byte codewords, without RTP. SBC has RTP and its
  SBC media header. The receiver validates framing and configuration, rejects
  fragmented SBC, and resets on sequence gaps instead of playing stale PCM.
  LHDC uses RTP plus `[frame_count << 2, packet_sequence]`; fragmented packets,
  unsupported frame formats, mismatched frame counts and truncated tails are rejected.
- Core 1 publishes decoded packets through a single completed-packet
  mailbox. Eight compressed packet slots bound the cross-core handoff; they have
  no playout timer. The controller driver can deliver a batch before core 0
  consumes the mailbox. The former two-slot queue rejected valid packets during
  these batches. A full queue now drops only the arriving packet and marks the
  next accepted packet for a decoder reset after the older queued work finishes.
  It does not clear valid queued PCM or repeatedly reset for each rejected packet.
  Decoded PCM uses the existing 8,192-frame pool as a 170.7 ms
  transient bound, with an independent 90 ms phone-input jitter reserve. Initial
  prefill is about 93 ms for SBC output, 100 ms for AAC-ELD and 111 ms for AAC.
  This candidate adds 10 ms to the 0.1.31 phone reserve; USB thresholds and its allocation
  are unchanged. Extra compressed slots provide burst capacity without a new
  startup or playout delay.
  A bounded resampling correction follows queue fill to compensate for independent
  clocks. Saturation discards the oldest complete PCM frames. These are buffer
  thresholds, not measured acoustic delay.
- Open the AirPods output before accepting the phone's Start. AAC-ELD's mandatory
  100 ms of encoded silence must be sent successfully before that acceptance,
  so phone audio cannot accumulate behind startup priming. Readiness has a
  three-second deadline; failure rejects Start rather than claiming playback.
- After one second without USB input, phone preparation/playback or HQ microphone
  demand, the output requests Suspend and waits for confirmation. New input
  restarts a locally suspended stream; AAC-ELD resets and repeats its 100 ms prime.
  USB PCM arriving during that restart is retained. Each transition has a
  three-second deadline; phone Start waits until restart and priming finish.
- Phone AAC-ELD output groups two 10 ms frames per packet when the negotiated
  payload fits them. Encoding still yields after one frame per main-loop pass.
  This halves media-packet frequency and adds 10 ms of packet holding versus
  0.1.30. USB retains its existing packet grouping. Output profiles offer discrete supported AAC/ELD bitrates;
  an explicit unsupported choice never falls back silently. Profile changes
  require idle audio and reopen AVDTP for negotiation. Timestamps, startup
  priming and controller flush policy remain fixed. The
  [profile contract](CONTROL-EXTENSIONS.md#output-profiles) lists valid choices.

The SBC demo's 60–80-frame buffer would hold 160–213 ms at 48 kHz. It is not used.
Small codec frames alone do not establish end-to-end delay: Android's batching,
radio scheduling, resampling and AirPods rendering must also be measured. aptX
to AAC-ELD is still a lossy transcode.

## Memory and ownership

Bluetooth, USB, playback encoding and PCM publication run on core 0. Core 1 owns
phone LHDC/aptX/SBC decoding or USB microphone decoding; the two inputs are mutually
exclusive. The phone worker allocates 15,972 bytes only during phone sessions or the
silent startup check. The LHDC decoder's ARM32 preparation requests 11,264 bytes
live and peaks at 13,792 bytes; its static state is included in the linked heap budget.
An idle microphone decoder is detached by core 1
before core 0 releases its heap for a phone session. USB recording can initialize
it again after the phone worker acknowledges detachment and core 0 frees its memory.
Both worker inputs and completed output carry a cancellation generation. A
partial or stale decode cannot enter playback. Codec allocation/free stays in thread context
under the Bluetooth lock; no packet callback allocates memory.

The phone and AirPods use independent AVDTP and AVRCP connection IDs. Shared
AVDTP discovery is filtered before the sink framework sees AirPods events.
Two Classic links fill the controller connection pool; passive BLE scanning uses
no connection slot. A connected BLE case would require a separate capacity review.

## Validation and remaining checks

Native sanitizer tests cover negotiation, framing, stale events, pairing-window
ownership, startup gating, bounded queues, controls and decoder release. Eighteen
ten-minute clock simulations cover the three output frame sizes at ±300 ppm,
including fixed-packet arrival drift. Real
LHDC/aptX/SBC codec tests run natively and in ARM32 emulation. The startup gate is
tested with synthetic clocks, including real decoder/worker integration; those
clocks are never used for performance claims. LHDC quality fixtures use original
synthetic PCM encoded identically by pinned C and official AOSP Rust encoders.
Gain, channel separation, malformed data and allocation cleanup are checked.
The firmware build repeats the original-PCM gain/error checks against its exact
linked LHDC archive. Silent, scaled or swapped output cannot pass that gate.
Firmware cross-compilation
checks the pinned SDK API and RAM layout. These checks do not establish radio
reliability, CPU headroom or acoustic latency on the Pico.

The phone jitter regression also uses production PCM, clock feedback and the
real resampler for three-frame/15 ms LHDC packets delivered in 20 ms batches.
After two minutes of settling it injects a 72 ms ingress gap and a 60 ms encoder
stall, separately and together, at ten encoder phases. The old buffer loses
samples; the current reserve and full existing pool pass this synthetic case. These tests
and the real decoder safety checks run in the float CI job after SDK preparation.
Typed feature 15 and `audio_diagnostics.py --phone` expose queue, decoder,
sequence-gap and reset counters without enabling per-packet logging. On 0.1.27,
an eleven-second hardware interval recorded 20 rejected submissions and 20
resets, with no new PCM underflows, PCM drops, decode failures or RTP gaps.
This distinguishes compressed-queue loss from the original PCM capacity issue.
The worker regression replays a short relative-arrival excerpt from that capture
with 7, 11 and 13.8 ms decode costs and 1 or 3.6 ms mailbox polling. It checks
ordered stereo delivery through the burst and subsequent backlog drain. A
separate saturation case verifies that older queued audio survives and decoder
history resets once at the first post-loss packet. These are deterministic
production-worker tests with a codec substitute, not on-device speed measurements.

The remaining 0.1.28 failure was downstream: an approximately 101 ms send wait
could silently discard phone scheduling debt above 60 ms while retaining its
old PCM. Version 0.1.29 reconciles an overrun by skipping the same number of
available PCM frames and owed frames, with explicit counters. If PCM is still
being decoded, the unmatched debt remains until it can be reconciled.

An AAC-ELD send gap longer than the receiver's current buffer window now enters
confirmed output Suspend/Start recovery for phone input too. The phone connection
is retained only for the same output connection and selected peer, for at most
three seconds, without USB/microphone takeover or a profile change. The decoder
can continue validated input during this scoped recovery. Start resets the
phone ring, clock and resampler; the 4,800-frame encoded warm-up runs before
the phone prefill gate. Normal readiness resumes only after that warm-up.

The existing CDC `t` timing request also prints one `[CREDITS]` line: the
controller's raw ACL count/length, the configured effective limit, current shared
free/outstanding slots, reserved packet-buffer state, and AirPods/phone role
completion counts and maximum active intervals since HCI initialization.
The phone's completion counts describe
outgoing control traffic, not incoming music acknowledgements. These counters
do not enable packet logging. A long completion interval is cadence evidence;
it does not by itself establish a radio stall or exhausted credits.

On 0.1.29, an on-demand snapshot reported eight physical controller ACL slots,
an effective firmware limit of three, zero free slots, three outstanding packets
and no reserved host packet buffer. Version 0.1.30 changes only the outgoing
limit to eight for the next comparison, retaining the 250 MHz clock, three
incoming host slots, audio framing and bitrates. BTstack still clips the limit
to the controller's advertised capacity. The older shared-bus warning remains
a reason to check bus writes/resets and USB latency; this is a controlled
experiment, not established proof that the three-slot limit caused the faults.
The initial approximately 44-second active interval then had no new PCM or
input losses or recoveries, although the listener still heard crackle. A later
snapshot showed a 59 ms maximum send wait, versus approximately 100 ms before,
and some subsequent losses. The next two-unit packet comparison retains that
window, clock and bitrate to test packet overhead separately from CPU speed.

On 0.1.31, the first approximately 47 active seconds had 299 zero-filled frames
and no drops, rejected input, decode errors or RTP gaps. A later snapshot showed
4,451 zero-filled frames and 217 drops, with no transport recovery and an 8 ms
maximum send wait. The remaining fault therefore needs input-timing investigation
as well as the earlier output corrections.

An integrated replay now runs the production phone worker, PCM publication,
real SDK resampler and playout path against three uninterrupted timing-only
capture segments, after clock-feedback settling. It reproduces up to 387 missing
frames at 80 ms. Near-instant decoding still reproduces the fault. The source
can accumulate 86.84 ms of lateness despite a 72.22 ms maximum individual gap;
the previous isolated-gap simulation missed that waveform. A 90 ms reserve passes
the phase/cost sweep without rejects or FIFO drops; 87.5 ms still fails.
Its smallest modeled low-side margin is only 93 frames, so 90 ms is a controlled
hardware candidate. Larger reserves also risk consuming real high-side headroom.
Version-2 phone diagnostics measure actual Pico arrival/publication gaps and RTP
timestamp discontinuities to validate this tradeoff on the next connection.

Control tests cover fragmented/coalesced RFCOMM frames, encrypted saved-peer
checks, per-client RPC ownership, replay protection, blocked writers and cleanup.
Finder tests resolve the selected pair's IRK against rotating addresses, reject
other nearby pairs, preserve old-reading ages, and exercise lease/microphone
pause behavior. These are protocol regressions, not live radio verification.

On hardware, first verify simultaneous phone/AirPods links, negotiated codec and
clean stereo. Measure first sound and steady delay separately, queue occupancy,
drift and heap/stack use. Then test long playback, pause/resume, phone radio loss,
AirPods case return, AVRCP/volume, USB takeover and microphone recovery. Compare
aptX with an SBC-only phone and check passive BLE reception alongside both links.
Also read the boot log's LHDC `frames`, `max_us`, enable and cleanup results. A phone
without an LHDC encoder will use aptX/SBC regardless of the Pico result. Measure
core 0 encoding load and both stack high-water marks during sustained LHDC playback.

Sources: [pinned BTstack AVDTP](https://github.com/bluekitchen/btstack/blob/501e6d2b86e6c92bfb9c390bcf55709938e25ac1/src/classic/avdtp.c),
[pinned sink example](https://github.com/bluekitchen/btstack/blob/501e6d2b86e6c92bfb9c390bcf55709938e25ac1/example/a2dp_sink_demo.c),
[AOSP classic aptX transport](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/system/stack/a2dp/a2dp_vendor_aptx.cc),
[pinned aptX decoder](https://github.com/regularhunter/libfreeaptx/tree/476e2d56b0374c643c50bc37ab5cfee3012e2144).

LHDC sources: [pinned AOSP framing and configuration](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/c77db469de80f86660bc053bec4dee0c5d4b947c/system/stack/a2dp/a2dp_vendor_lhdcv5.cc),
[decoder provenance and local fixes](../3rd-party/lhdcv5/README.dongle.md),
[synthetic fixture provenance](../tests/lhdc/fixture.json).
