# Phone bridge

Bluetooth phone calls are disabled by default on the supported Pico 2 W build.
USB calls and Bluetooth music remain available. The HFP/SCO description below
documents an experimental implementation, not supported Bluetooth call audio.

## USB audio presence and Bluetooth selection

Starting with 0.1.44, USB boots as `CAFE:4005` with HID/CDC controls only.
Headphones and microphone appear only when the selected AirPods establish their
Bluetooth audio-profile signaling connection. A real loss of that connection
removes USB audio, even if a USB microphone was open. Ordinary music pause,
media Suspend/Start, Game Mode adjustment, microphone recovery and standby
ownership changes retain the connection and do not change the USB identity.

| AirPods connection | Input selection | USB interfaces |
| --- | --- | --- |
| Absent | Any | HID/CDC only (`CAFE:4005`) |
| Present | USB | Speaker, microphone, HID/CDC (`CAFE:4015`) |
| Present | Bluetooth phone selected | HID/CDC only (`CAFE:4005`) |
| Present | Already accepted USB microphone request | Audio retained; USB capture has priority over Bluetooth selection |

The existing 250 ms detach interval coalesces rapid connection changes. Controls
remain available after re-enumeration; clients must reopen their handles as with
the existing Bluetooth mode switch. USB availability does not depend on active
playback or owning the AirPods audio route, which avoids a circular dependency
when starting music or requesting takeover. The host decides its output route
when the audio device is removed or restored.

An authenticated Bluetooth music connection selects the wireless input,
even before codec negotiation or Play and while paused. The dongle briefly detaches USB and returns
as `CAFE:4005` with CDC and HID only, so Android cannot select its USB speaker
instead. Reconnect diagnostic handles after this change. The serial number and
control protocol remain the same. Pairing, AVRCP alone, and an idle HFP connection
do not select this mode.

Once selected, media setup failure, profile teardown, pause and retry retain
wireless mode for that phone connection. Only releasing the entire phone ACL
connection (or replacing that phone) clears the selection and restores
`CAFE:4015`, the normal S16/float32 speaker and unchanged microphone, if the
AirPods are still connected. A session
token prevents a later connection reusing the same address/handle from inheriting
old mode state. Bluetooth voice capture/cleanup also hold wireless selection. An already
accepted USB microphone request retains priority while the AirPods are connected;
merely ringing is insufficient
to remove a USB call's audio devices. Selecting Bluetooth while USB recording
is active remains subject to that priority.

This mode change addresses host routing, not Bluetooth radio delivery. The
[0.1.39 audit](AUDIT-0.1.39.md) documents the separate audio stalls and the narrow
overflow-accounting correction. Pixel testing confirmed that 0.1.40 removed the
USB sound card while CDC/HID remained usable, but its media-profile-only lifetime
restored USB during a failed startup. That fallback was captured and is the
regression addressed by 0.1.41. Pixel checks on 29 October confirmed control-only
USB with no sound card, persistence after disabling Media audio while the phone
ACL survived, and restoration after full Bluetooth disconnect. Media reconnection
removed USB audio again. The idle-HFP-only reconnect test was inconclusive because
media became enabled too; it is covered by the host regression, not that hardware test.

## Bluetooth recovery candidate 0.1.56

The later [0.1.57 offline investigation](VALIDATION-0.1.57.md) reproduces a
separate CYW43 driver fault: ring-full writes were discarded while reported as
successful. Its asynchronous transport repair passed the full build and
sanitizer checks, but has not been installed or accepted by listening.

[0.1.56 validation](VALIDATION-0.1.56.md) records the current R3 build and phone
deployment, bounded media-profile recovery after output loss, and retention of
phone catch-up deadlines while PCM is still available. It also documents pairing
RAM/ACL retirement and the new-selection/old-signaling race. These changes retain
the existing buffer targets, codecs, volume behavior and radio policy.

R1 still produced audible relay glitches. R3 passed its full offline checks and
returned the saved AirPods with AAC-ELD after reboot, but its physical case-return
result and relay listening acceptance remain pending. Earlier failed runs below
remain historical evidence.

## Bluetooth continuity candidate 0.1.42

The confirmed Bluetooth-only 0.1.41 run sounded good initially and then became
silent. USB alternate 0 and control-only identity persisted throughout. Between
47.36 and 68.35 seconds, 21.002 seconds elapsed but outgoing RTP advanced only
19.980 seconds. No individual send gap exceeded 49.34 ms, so the existing
single-gap recovery guard did not trigger. Both connections had the Pico as
central/master, with no controller flushes. That rules out split roles as a
necessary condition for this failure; it does not rule out radio stalls generally.

The incoming packet size changed from 720 to 960 decoded stereo frames, while
the input still delivered approximately 48,000 frames per second. A production
scheduler reproduction exposed feedback in the 0.1.40/41 overflow credit: one
early 960-frame packet into an almost-full ring could cancel every read that
would drain it. The corrected accounting consumes that credit only against work
already exceeding the catch-up budget, then expires the remainder. It never
cancels healthy future deadlines or carries priming losses into active playback.

The sender also measures cumulative shortage of successfully submitted audio
against elapsed time. Catch-up packets repay it. Once shortage exceeds the
current receiver reserve, the existing bounded Suspend/Start recovery establishes
a new receiver boundary. Tracking resets across input/stream changes, priming,
ownership loss and intentional Game Mode timing adjustments. No buffer-size,
clock-speed, scanning or radio-role policy change is included. Submission remains
distinct from actual radio delivery, and final listening validation is pending.

## Earlier evidence

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
Version 0.1.33 adds a phone-call bridge; its bidirectional voice path is also
awaiting a physical call test. Candidate 0.1.36 renews the bounded HFP retry
budget when the saved phone changes or an authenticated phone connection
returns, including changes observed while USB owns audio. A normal HFP release
on the same connection retains its backoff. This fixes the permanent lockout
after three attempts while the phone was away; it does not validate voice timing.
The separate app control service remains untested on Pico hardware.

```text
Phone -- LHDC v5 / aptX / SBC --> Pico 2 W -- AAC-ELD / AAC / SBC --> AirPods
```

The phone pairs with **AirPods Dongle** as an ordinary Bluetooth audio output.
The Pico owns the AirPods bond, Apple Device ID, AACP and output codec. Android
needs no vendor-ID modification. Prior to 0.1.40, an active USB playback or
recording stream took priority over phone audio, including when Android silently
reselected USB after a replug. The selection policy above replaces that behavior.

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
playback. USB media commands stay on the USB path. During ringing or an active
phone call, a supported stem press answers or ends the call through HFP. A BLE
settings interface is not implemented.

The settings service accepts legacy Call Management (`0x24`) values `00 03 00 00`
(double press to end a call) and `00 02 00 00` (single press to end). These do not
establish a complementary mute assignment. They are independent of the HFP
transport described below. Newer
Apple versions contain independent end-call and mute fields; do not flatten a
newer or unknown report into one of these legacy values. Complete modern writes
and physical call behavior through this dongle remain unverified.

Confirmed AirPods volume is mirrored to both hosts. The
[USB volume relay](USB-CONTROL.md#stem-volume) remains under revision in 0.1.38.
Both 0.1.34 and 0.1.36 failed Android volume listening: the latter partly followed
the phone UI but retained an audible second gain step and possible divergence.
Fresh Pixel and Windows checks remain required.
A host volume request remains
an intent until the AirPods confirm it; temporary AVRCP busy states retain the
latest request. A controls-only connection or input switch does not replay a
reported level as a new request. New AirPods connections start with unknown
volume until fresh readback arrives. USB mute preserves volume and silences PCM.

## Audio ownership and takeover

The dongle already advertises Apple's Bluetooth Device ID (`004C:0000:0000`).
Version 0.1.38 adds the separate ownership exchange used by LibrePods; spoofing
the Device ID alone did not implement switching. Two concurrent host connections
are a protocol capability, not a claim that this candidate has passed a
two-dongle listening test.

With a saved AirPods pair, a single BOOTSEL press requests takeover. A double
press still toggles Game Mode, and the existing long holds still open pairing.
The button claims the audio route; it does not send Play to a paused player.
Already-playing input can resume once ownership and the stream are confirmed.
The routing exchange uses Control Command `0x09`, property `0x06` (ownership),
Audio Source `0x0E`, Connected Devices `0x2E`, and targeted routing messages
`0x10`/`0x11`. The builders and address order come from LibrePods' `AACPManager.kt`
and its `AirPodsService.kt` takeover path. Exact wire fixtures and malformed
length/address cases are covered in `aacp_routing_test.c`.

The request sends ownership, then the captured media and Hijackv2 messages when
another host is known. It waits up to ten seconds for positive remote ownership
evidence. It does not treat a locally queued or transmitted request as success.
Unknown routing stays passive, including at startup; standalone automatic start
therefore needs a valid local-owner/source or sole-local device report. Older
models and cold standalone startup require hardware verification.

A confirmed competing owner requests Pause from this dongle's active music host and
suspends its outgoing media while retaining the control connection. Fresh USB
input cannot override that loss. Volume, microphone acquisition and Game Mode's
receiver-buffer writes also stay passive. A takeover invalidates stale gain:
only a new host volume SET deferred during standby or a fresh native AVRCP
readback can establish the next baseline. Active calls are not answered or ended
as a side effect. A standby dongle does not automatically claim ownership for an
incoming Bluetooth call; existing preparation timeouts release an unusable voice
link. Physical takeover is blocked while local capture is requested. Call handoff
remains unverified. Manual `t` diagnostics include
a bounded `[ROUTING]` state line without printing routing traffic continuously.

In 0.1.43, phone takeover Pause takes priority over old queued media commands.
Input stops immediately; media teardown waits for AVRCP operation completion
or a two-second deadline. An unresolved command is canceled on its exact control
channel, and fresh Start cannot race it. Automatic USB Pause uses explicit
Consumer Pause (`0xB1`), expires if delayed, and is canceled when ownership
returns. Intentional stem Play/Pause remains `0xCD`. Android's generic mapping
may ignore explicit USB Pause; no toggle fallback risks resuming an already
paused player. Command delivery does not prove the host application paused.
See the [re-audit](AUDIT-0.1.42.md) for the reproduced failures and host limitation.

LibrePods connects to the saved phone's encrypted, bonded RFCOMM service for
device settings, hearing profiles, saved checks, audio profiles, performance
readings and nearby finding. See [control extensions](CONTROL-EXTENSIONS.md) for
the UUID and wire format. These controls do not need a PC, a phone vendor-ID
override or an audio stream.

## Phone calls over Bluetooth (disabled by default)

The default build does not register HFP, advertise its service, connect to the
phone's HFP service, or accept incoming SCO/eSCO links. This also rejects a phone
attempting to reuse cached call-profile information. USB call routing and the
Bluetooth music/AVRCP profiles are unchanged.

[Raspberry Pi issue 1461](https://github.com/raspberrypi/pico-sdk/issues/1461)
documents a CYW43 firmware failure during SCO connection setup. On 30 October
2026, a Bluetooth call attempt on this dongle left the shared controller bus
blocked for over 130 seconds with zero SCO receive/transmit packets; a reboot
restored it. The pinned SDK blob identifies itself as
`CYW4343A2_001.003.016.0065.0000_Generic_SDIO_37MHz_wlbga_BU_RPI_dl_signed`,
matching the base `.0065` firmware reported in the issue's February 2026
reproduction. This is closely matching evidence, not proof of the exact failed
controller event: no packet capture of that setup attempt was available.

For controlled firmware research only, configure CMake with
`-DAIRPODS_ENABLE_BLUETOOTH_CALLS=ON`. CMake passes the numeric
`AIRPODS_ENABLE_BLUETOOTH_CALLS` definition to the firmware sources; absent a
definition, `phone_call.c` also defaults to disabled. Enabling this option can
stall Bluetooth until reboot and does not make calls supported. Host tests run
the production call module both with its default disabled policy and with an
explicit opt-in for the existing HFP/CVSD tests.

With that experimental option enabled, the Pico advertises an HFP Hands-Free service on RFCOMM channel 8; the companion
control service keeps channel 7. The saved phone uses standard CVSD (8 kHz)
mono voice over SCO/eSCO. The Pico converts downlink voice to its
48 kHz stereo AirPods output and sends the AirPods AACP microphone back through
the selected call codec. Both companion apps can remain closed.

Only the current HFP session on the saved, encrypted phone connection may open
voice audio. Pairing mode alone never permits SCO. A call does not dial or
answer itself. Use the phone's call interface, or a supported stem press to
answer an incoming call/end an active call. Call-waiting, conferencing and
voice-assistant commands are not advertised.

USB playback or recording has priority over call audio; it releases the phone's
voice route without ending the telephone call. The caller can use the phone's
own audio route. Phone music decoding is released before the AirPods microphone
decoder is allocated. USB and call capture have explicit, exclusive ownership;
changing owners invalidates queued microphone samples. Calls use the existing
normal-buffer microphone policy and playback cleanup, preserving the requested
Game Mode for music afterward.

The converters and queues are bounded, with no per-packet logging or allocation.
Corrupt/missing voice is concealed or silenced; a malformed packet cannot be
treated as valid PCM. The microphone's USB format remains unchanged.
Call gains are local attenuation from mute to unity; they do not overwrite the
AirPods' confirmed absolute volume. The microphone converter uses averaged
queue occupancy for continuous clock correction bounded near ±1,000 ppm.

Wideband mSBC is not advertised. Its additional decoder/encoder state exceeded
the linked codec-memory gate's required 64 KiB runtime reserve. A real-SDK
framing/reset regression remains in the codec suite, including a hash-checked
fix for the pinned SDK's partial decoder initialization; passing that test does
not enable wideband calls on the board.

**Hardware gate:** build and host tests cannot prove CYW43 voice routing or
full-duplex radio timing. Keep the default disabled until the controller firmware
limitation is resolved and verified on this board. Before calling this supported,
verify incoming and outgoing calls, audible speech in both directions, call
answer/end, USB takeover, call end followed by music, and radio loss/reconnect.
No end-to-end call latency has been measured.

## Phone calls over USB

With the Pico connected to a phone's USB port, the phone sends call playback
through the USB speaker interface and opens the USB microphone interface for
uplink. This path uses neither HFP nor SCO between phone and Pico; both
companion apps can remain closed. Active USB playback or capture prevents the
Bluetooth call bridge from acquiring audio. Closing capture performs the same
microphone drain/playback repair as other USB recording sessions.

The USB microphone remains 16-bit mono at 64 kHz, with its existing direct PCM
delivery. Captured Pro 3 runs decoded about 132–136 units per second, each
containing 480 mono samples, consistent with that presentation clock. The
hardcoded AAC decoder ASC reports 48 kHz; this coding configuration alone does
not establish the physical sample cadence or justify a USB rate conversion.
The Bluetooth call uplink therefore converts from the measured 64 kHz
presentation clock to CVSD's 8 kHz. Its drift correction handles small clock
differences, not a 48/64 kHz mismatch. Playback remains a separate 48 kHz clock.
See [microphone cadence evidence](AUDIO-CONTINUITY.md#microphone-coexistence-investigation-0122-s16).
USB and Bluetooth consumers never share capture ownership.

The existing headphone/microphone terminals and HID interface satisfy
[AOSP's USB headset classification](https://android.googlesource.com/platform/frameworks/base/+/master/services/usb/java/com/android/server/usb/descriptors/UsbDescriptorParser.java).
Stem commands retain Consumer Play/Pause (`0x0c:0x00cd`), which Android's
[USB headset specification](https://source.android.com/docs/core/interaction/accessories/headset/usb-device)
and [Telecom handler](https://android.googlesource.com/platform/packages/services/Telecomm/+/master/src/com/android/server/telecom/HeadsetMediaButton.java)
use for answering/ending calls. The phone interprets the current call context;
the Pico does not infer a USB telephone call merely because a microphone opens.

Actual Pixel telephony routing, microphone speech quality, stem behavior during
a call and return to music still require a physical USB call test. A media-only
or Symfonium-exclusive playback test does not validate communication capture.
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

The 0.1.39 Bluetooth-only audit still reproduced audible glitches with Game Mode
off. During one 4.371-second interval, input decoding produced 210,960 frames
without new sequence gaps, rejects or decode errors. Output discarded 41,376
PCM frames, matching its discarded scheduling debt, and the controller reported
45 flush events. These locate that interval's loss downstream of the decoder;
they do not establish whether the original send delays came from radio scheduling
or host processing. The four timeline-recovery counts were trimming episodes,
not four connection restarts.

The audit also found a separate accounting defect when decoded input physically
overfilled its ring: those discarded samples remained owed by the sender and
could be discarded again by the backlog limit. Version 0.1.42 matches physical
loss only against current debt above the catch-up limit and immediately expires
unused credit. The earlier 0.1.40/41 implementation also cancelled healthy future
deadlines, creating the no-output feedback reproduced above. Resetting or priming
the buffer clears its pending accounting. Explicit trims create no new credit, and
overflow cannot shorten the mandatory startup silence. USB policy, phone reserve,
clock correction and receiver deadlines are unchanged. A production-path test
checks the former 320-frame overflow followed by a 620-frame trim: only 620 total
frames should be discarded, rather than 940.

This correction does not claim to resolve the observed wireless stutters.
Disposable replays with the real worker and SDK resampler added 76 ms send waits
to the captured arrival timing. Raising the phone debt limit from 60 to 70–80 ms,
or relying only on physical overflow, reduced loss but still produced drops or
zero-fill at some timer phases. Freezing clock correction during the backlog also
failed clean-audio acceptance. Those policy changes were rejected. The existing
90 ms input reserve and 8,192-frame pool cannot be treated as unlimited protection
against simultaneous input jitter and outgoing stalls; controller credit/delivery
evidence and another controlled hardware comparison are still needed.

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
