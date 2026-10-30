# USB audio continuity and float transport

## 0.1.54 precision candidate, 30 October 2026

R6 adds integer PCM32 USB alternate 3 and removes the S16 conversion before
AAC-ELD. Its CBR bandwidth is explicitly 20 kHz; the frame size, buffering,
clock controller, priming and volume paths are unchanged. Details and exact
build identities are in [the codec protocol notes](../aac-eld-apple.md#0154-playback-precision-and-bandwidth).

The full build, sanitizer, real-codec, volume SDK, phone-codec, LHDC and ARM
memory checks passed. The worst emulated allocation leaves 66064 bytes, retaining
the required 65536-byte reserve. Both physical dongles were loaded with the same
verified UF2; each flash used a private full backup and byte comparison of the
last 64 KiB before reboot, preserving saved pairing storage.

The PC's bounded quiet-reference runs encoded real audio in normal mode and
through Game Mode on/off without new active discard, zero-fill, bad USB input,
short read, scheduling-debt discard, timeline recovery or controller flush.
The observed encoder maximum was 6301 us; the largest complete encode-unit
observation across normal/Game checks was 8005 us, below one 10000 us ELD frame. Physical
gain stayed at the original 119/127. The old baseline's lifetime maximum is not
an isolated CPU comparison. Requested buffer settings and delay reports do not
measure final acoustic latency.

The first R6 attempt with two hosts connected was passive and encoded no audio;
exclude it from successful playback evidence. Isolation and a fresh connection
were required for the subsequent PC measurements. Total PCM-drop counters during
start/stop were nonzero in both old and new firmware; the active counters above
remained unchanged. Do not describe the entire experiment as lossless playback.

Android live PCM32 and listening checks remain open. The phone dongle could not
page the AirPods after reconnect, including with the known-good 0.1.53 control
image and identical saved pairing storage. R6 was restored afterward. This blocks
its live-format check; compiled alternate-3 descriptors alone are not proof that
Android actually selected S32_LE. Resume after a case cycle with awake buds.
No microphone recording or listening acceptance occurred overnight. The native
microphone's S16 USB interface is deliberately unchanged.

## Acceptance status, 28 October 2026

Firmware 0.1.36 fixes a reproduced waveform discontinuity in USB playback.
With a fractional clock phase, a 480-frame output unit can need 481 input frames.
The old path fell back to unfiltered PCM when only 480 were available, then
returned to the delayed FIR path on the next unit. A synthetic ramp jumped by
115 sample units at that boundary while both underflow counters remained zero.
The new path preserves FIR history and fractional phase, pads missing input in
that same path, and counts the shortage. Production-ingress tests cover S16 and
nonquantized float input. This reproduces a possible crackle mechanism, not the
initiating event of every user-reported hitch. Steady Game Mode listening remains
a separate acceptance check from the already accepted brief transition pitch ramp.

The first 0.1.36 Pixel USB listening check was clean in both ears with Game Mode
on, phase 3 and a reported 60 ms receiver delay. The phone selected S16 alternate
1, not float. A short snapshot showed zero active dropped/zero-filled frames and
no recoveries; the full encode unit maximum was 7,258 us at that point. This is
an initial playback result, not extended acceptance. Its volume comparison
failed: the listener heard the gain repair as a second step and could still
desynchronize phone and stem volume. Keep that failure separate from playback.

The first 0.1.37 Pixel USB attempt was silent in both ears. USB S16 frames arrived
and were trimmed as startup input while encoded units stayed at 853 and the
Bluetooth streaming flag was clear. All eight controller credits were free.
A case return restored clean stereo. Source regression reproduced an unsolicited
peer SUSPEND leaving the idle phase RUNNING with streaming false, so even a fresh
USB stop/start did not issue Start. The initial peer SUSPEND was not captured on
hardware; it remains a candidate explanation for this observed stopped state.
The later stem-volume test caused a repeatable left/right imbalance that cleared
on pause/resume; see [volume evidence](VOLUME-EVIDENCE.md).

Two audit limits remain: sustained 10 ms work per AAC-ELD unit cannot support
the temporary 9 ms buffer-fill schedule when leaving Game Mode; and the host
pipeline model treats successful HCI submission as immediate receiver delivery.
Do not fix the first by increasing clock or discarding audio, or claim the
second proves the 60 ms receiver buffer cannot underrun. A complete busy-ramp
admission policy and a controller-credit/delivery replay need separate validation.

Full acceptance remains open. The 0.1.25-r1 Windows S16 and actual IEEE float32
USB checks passed the listener's short playback/microphone comparison. Stereo
returned normally, and the float mic result was rated the same or better than
the accepted S16 result. An opening resync and the planned post-stop repair remain;
this is not evidence of continuous microphone transitions or a 30-minute pass.
Current Android normal playback and Symfonium USB-exclusive checks are pending.
The detailed results below retain the earlier failures and their limits.

Crackle was reported during steady playback on both Windows and the Pixel 10
Pro XL. Firmware 0.1.19 was rejected after the listener
reported substantially worse sound. The listener switched to speakers and
declined rollback flashing. The later 0.1.20 S16 comparison initially sounded
good, then became silent without disconnecting. Do not interpret
passing host tests, an accepted Bluetooth send, or a 60 ms delay report as proof
of clean playback. The verified 0.1.18 rollback remains available locally.

The 0.1.20 code adds float transport, a higher-quality fractional resampler and
further diagnostics. It failed sustained hardware playback. 0.1.21 corrected
the measured CPU pacing deficit, but also failed listening: distorted audio,
silence, one watchdog-based restart and poor out-of-range recovery were observed.
The old reset message did not distinguish an expired watchdog from a requested
watchdog reboot. Replugging restored clean playback temporarily; reconnecting
the buds alone did not reliably recover it. The cause of every audible failure
has not been established. The 0.1.22 S16 candidate improved steady playback and
range recovery, but microphone coexistence failed. It is not an accepted audio
release. The 0.1.23 candidate improved throughput during microphone use, but failed
stereo listening and later reset. The 0.1.24 candidate added coordinated mic
startup/shutdown recovery, further codec placement changes and serialized
BOOTSEL sampling. Its short hardware results and the subsequent 0.1.25 comparison
are recorded below.

## Evidence

- 0.1.21 music playback initially held approximately 100 encoded units/s. One
  early 40 ms send wait coincided with 1,571 active discarded frames and a local
  timeline recovery. A separate 59.055-second scanning-off comparison held
  100.008 units/s but inserted 301 zero-filled frames. Scanning was restored.
- After a subsequent restart, more than five minutes were heard clean with no
  active source loss. Going out of range then produced multiple local recoveries,
  continued packet submission and prolonged silence/garbling on return. Packet
  submission and controller completion do not establish successful radio delivery.
- A new ten-minute simulation with a timer consistently 3 ms late reproduces
  the old clock controller freezing: its normal scheduling debt was mistaken
  for a transport stall. Clock correction now subtracts already owed work and
  elapsed scheduling phase from measured occupancy before averaging it. It
  freezes only when a second full unit is owed, retaining the same correction
  bounds. A separate test holds the measured sender wait to 40 ms with encoding and
  timer phase included. The previous post-request simulation actually added the
  encoding duration on top of that wait, exceeding the measured 40 ms budget.

- During 0.1.20's silent failure, USB delivery continued near 48,000 frames/s,
  with no malformed packets, zero-fill, failed sends or controller flushes.
  Encoding fell to approximately 98 units/s instead of the required 100.
  A separate 19.029-second scanning-off window still produced only 98.38 units/s,
  discarded 14,828 active PCM frames and performed ten timeline recoveries.
  Scanning was restored automatically; it did not explain this failure.
- The complete PCM/filter/encode unit reached 9,646 us and the Bluetooth main
  pass reached 10,923 us. The main loop then unconditionally slept another
  millisecond. The 0.1.21 loop paces only short passes to one millisecond, releasing
  the Bluetooth lock between passes without an extra sleep after expensive work.
  The integration model now includes filtering and other loop costs, rather than
  treating the mock encoder's 5–8 ms as the entire CPU budget. This is an observed
  source starvation defect; receiver behavior and clean listening still require
  validation. Continuing USB packet counts alone do not prove nonzero source PCM.

- The earlier 0.1.17 phone capture accumulated 816 underflow frames and 864
  discarded frames over 59.39 seconds, with zero reported send failures.
- The one-minute 0.1.18 PC baseline contained a user-reported crackle but no new
  PCM underflows, discarded frames, send failures or recovery requests. The
  reply-time marker is approximate; it is not an instrumented acoustic timestamp.
- The 120-second 0.1.19 PC capture had zero active zero-fill, 251 initially
  counted discarded frames, two local timeline recoveries, send waits up to
  32 ms, and no reported controller flush. That build undercounted additional
  PCM trimmed during recovery; the new code counts that trimming too.
- 0.1.19 USB service maxima remained within the existing 500 us budget. Its
  rejected sound cannot be attributed solely to USB packet loss from those data.
- Two-point interpolation has a separate reproducible spectral defect: at
  half-sample phase it attenuates high frequencies. Tone tests through 20 kHz now
  require more than 50 dB reconstruction SNR after filter startup. This establishes
  the numeric improvement, not the cause of every audible hardware fault.

Private raw captures remain in the local bring-up output directory. No hearing
profile, volume, codec preference or paired identity is changed by diagnostics.

## Buffering and scheduling

At 48 kHz, ELD encodes 480-frame units. USB startup retains at most 1,440 frames;
normal clock adaptation targets 960 remaining frames after a read. The running
queue may temporarily reach 3,840 frames to absorb send backpressure, an encoded
unit and scheduler phase. The original 2,880-frame cap lacked this processing
headroom in the measured hardware run. This is a capacity limit, not a permanent
80 ms prefill. Fallback codecs reserve at least
this capacity and extend it when four codec units exceed 40 ms. Larger stalls reconcile PCM loss
and elapsed sample debt in one counted recovery. Lost source time now triggers
the existing confirmed Suspend/Start path rather than silently continuing the
old receiver stream. Both codec state and media clocks reset at the confirmed
boundary. Failed Suspend, rejection or a missing Start confirmation falls back
to disconnect and saved-pair reconnect. This recovery path still needs hardware
verification; the exact receiver-side failure mechanism is unproven.

Timer and send callbacks schedule work. The main-loop worker encodes at most
one ELD unit per pass. Absolute timer deadlines and fractional sample debt are
retained. The established RTP sample clock and AAC-ELD headers are unchanged.

Normal USB clock correction uses a 320 ms occupancy average and +/-64 Q16 steps
(approximately +/-977 ppm), with a 120-frame dead band. It freezes during
backpressure and Game Mode's finite 0.9/1.1 adjustment. Phase and filter history
survive block/rate boundaries. Changing rates first accrues elapsed time and
converts the existing sample debt into the new rate domain instead of clearing
it. An integration test with jitter and repeated live changes reproduced source
loss before this correction and completes without that discontinuity afterwards.
A 33-tap, 128-phase windowed-sinc filter adds 16
frames of delay (0.33 ms); its lower-bandwidth table applies during 1.1 source
consumption. Regenerate coefficients with `python3 tools/make_resampler.py`.
The worker copies its bounded source window while excluding USB IRQs and performs
filtering afterwards with interrupts enabled.

## USB formats

Speaker interface 1 exposes alt 0 (idle), alt 1 (S16_LE stereo/48 kHz) and alt 2
(IEEE float32 stereo/48 kHz). Their full-speed maximum packets are 196 and 392
bytes respectively, at a 1 ms interval. Each alternative advertises one format.
The microphone remains S16_LE mono/64 kHz on interface 2, alt 1, with a 130-byte
maximum packet. CDC, HID media controls and settings collections retain their
interfaces. The linked descriptor is checked before UF2 publication.

Float PCM is preserved in the shared ring and fractional filtering. Existing
integer inputs normalize to float. Nonfinite samples become silence; finite
headroom is retained until rounding and saturation into the unchanged integer
FDK/SBC input. This is not a float Bluetooth codec. Format changes clear obsolete
source data and reset the USB clock state. Diagnostics report the actual selected
USB alternative, rather than inferring it from a host mixer or source file.

Release firmware enables float and retains S16 alt 1 compatibility in the same
image. CI publishes that single image; regression tests exercise both formats.
The earlier S16-only comparison images were used during the initial float rollout.

### Transport quality checks

A quiet eight-second Windows reference on 0.1.53 float r2 preserved all 384000 stereo
frames (768000 float samples) bit for bit in captured USB OUT request payloads
paired with successful completions. Both channels had unity amplitude and zero
sample error, including the sub-S16-level section and the 19/21 kHz test tones.
This is host USB driver evidence, not a physical-bus analyzer or an encoder-output
measurement. Two failed transfers occurred after the complete reference window;
the whole capture is not labeled error-free. Evidence: `pc02-quality.json` and
`pc02-stop-tail-validation.json` in the `usb-pcm-quality-20261029` artifact set.

The Android trial accepted and played the same float AudioTrack reference without
a reported underrun increase, but its saved USB selection was alt 1 / S16_LE.
The saved PCM status was stopped, and no USB audio payload was captured, so
neither Android sample fidelity nor float USB transport is established by that
trial. An offload path is a possible explanation for absent USB-monitor events,
not a demonstrated cause. At the time of this 0.1.53 test, a standard S32 integer
alternative was only a candidate and the encoder still had an S16 boundary.
0.1.54 implements both wider input and the Q31 encoder path; its actual Android
route remains unverified as described above.

For repeatable quality checks:

1. Use known, quiet 48 kHz stereo material with a unique alignment marker,
   separate channels, low-level samples off the S16 grid and high-frequency tones.
   Pause other audio sources; do not compensate for gain by changing live volume.
2. Record the selected USB alternative and its format. Filter capture to the
   intended audio OUT endpoint and match submissions with successful completions,
   including per-packet status. A float source or mixer format alone is insufficient.
3. Align the complete reference and compare sample bits, channel order, gain,
   nonfinite/clipped samples and frequency components. Keep transfer errors and
   their positions; do not hide an error by concatenating around missing audio.
4. If firmware configuration is observed separately, identify the exact build and
   report that as a separate observation. Record effective encoder bitrate,
   bandwidth, SBR and afterburner, not just requested settings.
5. Stop bounded collectors afterward. Retain inconclusive captures as such rather
   than treating absent packets or a mixed reference as a successful comparison.

The PC result establishes preservation only within that quiet reference window.
It does not establish full-scale clipping behavior, prepared encoder PCM,
decoded AAC fidelity, acoustic response, or long-run continuity. In 0.1.53, float
was rounded to S16 before encoding; a separate runtime snapshot confirmed a
16000 Hz AAC bandwidth at 255200 bits/s CBR. These were distinct downstream limits;
see [AAC-ELD bandwidth and precision](../aac-eld-apple.md#encoder-bandwidth-and-precision).

## Trace protocol

Report-3 typed feature 11 returns exactly 512 bytes. All integers are little-endian.
It does not reset counters, drain event history or alter playback. Features 5
and 9 retain their existing layouts. The response fits the existing 512-byte RPC
storage and uses normal 42-byte paging; command 10 remains the settings lease.

| Offset | Contents |
| --- | --- |
| 0, 1, 2, 3 | Version 1, event size 16, event count 0..24, reserved 0 |
| 4..28 | Seven u32: snapshot microseconds, event serial, PCM used, sample debt, Game phase, RTP timestamp, reserved |
| 32..127 | Twenty-four u32 statistics below |
| 128..511 | Up to 24 chronological events: u32 microseconds, kind, argument A, argument B |

Statistics, in order: USB packets; USB frames; maximum USB arrival gap in us;
bad USB packets; short USB reads; startup discarded frames; active discarded
frames; startup zero-fill; active zero-fill; maximum timer lateness in us;
maximum encoder time in us; maximum send wait in us; maximum send gap in us;
maximum packet age in us; controller flushes; controller completed packets;
discarded sample debt; local timeline recoveries; minimum PCM used; maximum PCM
used; clock rate Q16; requested BLE scanning; selected USB alternate; maximum
full PCM-read/filter/encode unit time in us. The minimum is `FFFFFFFF` until a
normal USB resampled read. Controller counts cover all controller handles, not
only media packets. Maximum encoder time excludes PCM preparation; unit time
includes it. Startup and pause gaps can appear in lifetime maxima, so compare
window deltas and events during confirmed steady playback.

Event kinds: 1 drop (frames, occupancy); 2 zero-fill (frames, occupancy); 3 late
timer (ms, debt); 4 delayed send (wait us, send gap us); 5 recovery (discarded PCM,
discarded debt); 6 flush (handle, 0); 7 USB error (bad=1/short=2, frames); 8 format
(alternate, 0); 9 reserved for Game transitions. Microseconds and counters wrap
modulo u32. History retains the last 24 events across serial wrap. No audio
payloads, Bluetooth keys or personal profiles are exported.

Typed feature 12 takes `enabled` 0/1 and returns empty. Zero pauses background
BLE scans for at most 120 seconds. One clears only that diagnostic pause. Normal
microphone, inquiry and suspend pauses retain priority. It does not make BLE a
required companion service or change the default scan policy.

## Collection and controlled comparisons

Typed feature 13 is an opt-in 400-byte signal/ELD inspection snapshot, available
only with AAC-ELD selected. It includes the latest AU from a packet accepted by
the transport, its AudioSpecificConfig, source/encoder levels and cumulative
nonzero-sample counters. It does not prove radio reception. Use `--inspect` with
the normal collector (explicit capture lease, disabled by default); keep this short encoded program-audio sample local when
sharing sanitized counters. See CONTROL-EXTENSIONS.md for the exact layout.

0.1.22 also records the last main-loop stage, USB IRQ context and ARM fault status
in watchdog scratch 0..3. A watchdog-expiry reboot reports `[HANG]` over CDC;
scratch 4..7 remain owned by the SDK. Requested watchdog resets are labelled
separately. This adds evidence; it does not repair or mask a hang.

Close companion apps to avoid competing HID transactions. On Windows:

```powershell
python tools/audio_diagnostics.py --seconds 60 --serial YOUR_PICO_SERIAL --output capture.jsonl
python tools/audio_profile.py --game off --seconds 60 --serial YOUR_PICO_SERIAL --output game-off.jsonl
python tools/audio_profile.py --scan off --seconds 60 --serial YOUR_PICO_SERIAL --output scan-off.jsonl
```

The collector is read-only. The separate profiling tool changes one test variable
and restores it on completion or a capture exception. Scan pauses also expire in
firmware if the host vanishes. Profile captures last at most 90 seconds. Game
Mode restoration needs a surviving connection; capture errors are reported.
The collector restricts reads to audio status/performance/trace, validates target
generation and paging, and refuses an already busy transaction. It works on
older firmware with `--legacy`. Files are created exclusively to preserve evidence.

On rooted Android with Termux Python and the Pico connected as USB peripheral:

```sh
su -c '/data/data/com.termux/files/usr/bin/python3 /data/local/tmp/audio_diagnostics.py --seconds 60 --output /data/local/tmp/pico-audio.jsonl'
```

The Python scripts need no third-party packages. Windows verifies CAFE:4015,
the FF31:0001 vendor collection and 64-byte feature reports; Linux/Android
requires exactly one matching hidraw interface. The microphone and hearing
settings remain untouched. Keep raw system logs private.

## Remaining hardware acceptance

For each path, use the same local 48 kHz passage at comparable volume and require
30 minutes without audible crackle, new active source loss or local recovery:
Windows normal playback; Windows explicitly selected IEEE float; Android normal
playback; Symfonium USB exclusive. Confirm the actual selected alternative on
each path; Symfonium float support is not assumed. Compare Game Mode and scanning
separately if faults persist with clean source counters.

Also verify live Game Mode switching, approximately 60 ms latency, stems, cold
USB startup, case-return reconnect and microphone coexistence with companion
apps closed. Inspect complete unit time, USB service timing and the linked
memory receipt before another listening trial. Preserve the rollback and pairing.
Restore the previous NTPods processes after standalone testing is finished.


A media submission gap longer than the selected 60/150 ms receiver window also
requests confirmed recovery from the main loop. Successful submission alone is
not enough to classify a stream as healthy after a long outage. The check is
inactive for startup/pause gaps because the send-history boundary resets with
the stream. Radio receipt remains unobservable through these counters.


## Initial 0.1.22-r5 hardware results

The installed S16 comparison passed a 358.564-second music interval with no
continuous diagnostic polling or frame capture. Start/end snapshots measured
99.9989 AU/s, zero new active discarded/zero-filled frames, no recoveries,
controller flushes, malformed USB packets or restart. The listener confirmed
clean audio throughout. Detailed inspection stayed disabled (zero samples
inspected), and the encoder generation remained unchanged. This is provisional
six-minute evidence, not the requested 30-minute or full device-path acceptance.

The physical BOOTSEL double press was verified disabling Game Mode, with three
seconds dark followed by solid. The listener heard a temporary pitch drop and
then normal audio. The sender's finite 9/10 adjustment completed in 789 ms without
source loss, recovery or reconnection. That temporary speed/pitch change is a
finite latency transition accepted by the listener on 28 October: a brief pitch
change is preferable to silence. It remains the intended live transition. The
enabling feedback, longer playback and float transport need further checks.

An out-of-range walk with Game Mode off subsequently returned clean audio
automatically, as confirmed by the listener. The 103.6-second snapshot interval
had no MCU restart and exercised a full Bluetooth reconnect fallback. Source
discard and recovery during that deliberate radio fault are not a clean
steady-playback interval.


## Microphone coexistence investigation, 0.1.22 S16

Opening the dongle microphone in Windows Sound Settings caused playback pauses
and eventual failure. The listener replugged the Pico afterward; that first
post-replug snapshot did not contain the actual microphone failure.

A later bounded 20-second WASAPI microphone open reproduced the playback deficit
without storing a recording. During an 18.98-second interior window, throughput
fell to 67.96 AU/s, with nine recoveries and 21,560 active discarded PCM frames.
Encoder maximum rose from 7,716 to 18,623 us; complete-unit maximum exceeded the
10 ms deadline. USB delivery remained valid. Mic decoding initially ran around
132--136 units/s with zero decoder errors or input-queue overruns. After closing
capture, playback returned to about 100 AU/s with no new recovery in the sampled
seven-second interval. No MCU reset occurred during this controlled window.

A separate mic-open comparison with Game Mode requested off also failed:
33.09 AU/s, fourteen recoveries and 34,608 active discarded frames over its
18.01-second interior. Mic queue overruns occurred in that run. Closing capture
again returned to about 100 AU/s. Its entrance included a Game Mode transition
and recovery, and its status was `GAME_FAILED`, despite the off request. It
therefore does not establish a clean Game Mode off baseline or fully isolate
that variable. The sustained microphone-related deficit remains reproducible.

The observed mic arrival cadence is approximately 133 units/s times 480 mono
samples, consistent with the existing 64 kHz USB microphone. A fixed decoder ASC
reporting 48 kHz alone is not evidence that the USB endpoint should change.
The microphone format is retained.

The CDC boot ring also retained an earlier genuine watchdog expiry:
`state=a1d70002`, checkpoint 366,074,883 us, CFSR/HFSR zero. It preceded the
controlled capture. Stage 2 covered BOOTSEL sampling and subsequent button
actions, so this does not isolate a lockout or button action as the cause.
0.1.23 separates those stages and uses atomic peripheral updates so concurrent
stage/USB markers cannot erase a core-1 fault flag. This improves evidence; it
does not claim to fix that earlier hang.

The 0.1.23 performance candidate moves six shared FDK math/transform objects
from flash to RAM using the pinned SDK linker layout. This changes placement,
not codec arithmetic or audio formats. The linked-ELF gate checks a live kernel
from every selected object and retains the 64 KiB runtime-reserve requirement.
Concurrent encoder/decoder flash-cache contention is a hypothesis; improved
on-device deadlines and listening are required to accept this candidate.


## Microphone coexistence, 0.1.23 S16 hardware result

Moving the six shared DSP objects into RAM improved a controlled mic-open
interval: 100.007 AU/s over 14.009 seconds after the initial recovery, with no
additional discarded/zero-filled frames, recoveries or flushes. The comparable
0.1.22 interval reached 61.287 AU/s and recovered eight more times. Mic decoder
mean time fell from about 2,653 to 2,076 us. A recovery still occurred about
3.2 seconds after opening capture, discarding 2,329 PCM frames and 2,928 frames
of scheduling debt. The complete encode unit reached 14,491 us.

The listening test failed: initial crackle and the latency transition were
followed by persistent right-only playback after closing the mic. Two short
accepted AAC-ELD AU snapshots decoded locally as valid 48 kHz stereo, with all
480 samples in both channels nonzero. This establishes valid source stereo at
those snapshots, not successful radio delivery or continuous audio in both buds.
Raw program-audio samples remain local and are not committed or uploaded.

The eventual automatic return to clean stereo was a Pico watchdog reset, not
merely a Bluetooth reconnect. The new boot marker was `a1d70010`, checkpoint
382,405,325 us, CFSR/HFSR zero: BOOTSEL SDK entry before the RAM sampling callback.
The new sampling guard serializes that operation with the Bluetooth background
context. Without it, a preempting TLV flash write can re-enter the same SDK
flash-lockout mutex. This closes a reproducible re-entry window; the specific
expired-watchdog instruction sequence was not captured.

Existing NTPods implementation and protocol research already distinguish quiet
microphone uplink from restored stereo. Its shutdown drains uplink, then repairs
playback, with bounded confirmation and a reconnect fallback. The Pico lacked
that post-stop repair and could start mic streaming before restoring the normal
150 ms buffer. The 0.1.24 lifecycle must coordinate both transitions and retain
the user's requested Game Mode. The reference is
`ntpods/docs/WINDOWS-AIRPODS-AUDIO-PROTOCOL.md`, Microphone lifecycle and playback
recovery; the old Alternative A2DP evidence records the same one-channel symptom.

Native encoder profiling with a synthetic stereo fixture ranks quantization,
bit counting, dynamic bit allocation and temporal noise shaping as the next
hotspots. The candidate moves their four ARM objects (21,920 bytes) and encoder
lookup tables (20,272 bytes) into RAM. Desktop profiling ranks work; it does not
establish ARM timing. The actual linked memory gate must still retain 64 KiB
before any flash artifact is accepted.

Tahoe's captured HFP/CVSD return resets outer RTP sequence as well as AU and
timestamp counters on the retained channel. NTPods/Pico currently retain the
outer sequence. The capture does not contain AACP 0x58, and earlier Windows
millisecond timestamp experiments failed; this observation does not authorize
an untested RTP-format change for this fix.


## 0.1.24-r4 initial microphone comparison, 28 October 2026

The S16 build started normally and reconnected to its saved Pro 3 pair. A
20-second WASAPI mic open received and decoded the microphone stream; this was
not a silent test caused by the readiness guard blocking Start. Across the
19.021-second observed capture interval, playback reached 99.9947 AU/s. The
16.018-second interior reached 100.0125 AU/s. There were no active discarded or
zero-filled PCM frames, timeline recoveries, controller flushes, malformed USB
packets or short reads. Uptime tracked elapsed wall time without a restart.
Encoder maximum was 6,970 us; mic decoding averaged about 1,877 us. A complete
unit occasionally reached 12,274 us, which the queue absorbed without growing
scheduling debt. Do not infer that every unit stayed below 10 ms.

After mic close, one deliberate Suspend/Start repair completed on the existing
Bluetooth connection, then Game Mode returned with its accepted pitch change.
The repair discarded about 570 ms of queued source audio; it was not continuous
playback across shutdown. The listener reported a substantial improvement and
localized the remaining crackle/pause to opening or closing transitions, with
no continuing one-ear failure reported. This is a short coexistence result,
not full 30-minute or microphone-recording-quality acceptance.

The subsequent steady run is being checked without detailed inspection or
continuous HID/serial polling. The BOOTSEL lock fix still needs sustained
hardware coverage beyond the previous watchdog interval. Float-capable firmware
is built from identical runtime sources, but real IEEE float USB transport,
Android normal playback and Symfonium exclusive output remain unverified.

Windows format checks must distinguish the engine mix format, endpoint device
format and selected USB alternate setting. Type I IEEE float requires 4-byte
subslots and 32-bit resolution; see Microsoft's USB Audio 2.0 driver reference:
https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers
A float mix format alone does not establish float transport to the Pico.


The subsequent unpolled music interval lasted 349.888 seconds: 99.9978 AU/s,
zero new active dropped/zero-filled frames, recoveries, controller flushes or USB
errors. Uptime advanced 349.874 seconds and reached 604.334 seconds without a
reset, beyond the earlier roughly six-minute watchdog failures. Game Mode was
off by listener choice. This is ten-minute uptime coverage and a five-minute-
fifty-second steady interval, not a 30-minute acceptance result.

## Startup filter continuity candidate, 0.1.25

A production-code ramp reproduces a 16-sample jump when the first real USB PCM
block bypasses the FIR but the next block uses its group delay: direct audio
ends at source index 479 and filtered output resumes at 464. Normal-mode
three-AU packets can defer the primed-on-wire flag until two real blocks have
already bypassed the filter after the ten silent priming AUs. This creates a
0.333 ms phase discontinuity at 48 kHz, a candidate cause of the transition click.

The filter now runs from the first real USB frame, while occupancy clock feedback
still waits for priming to reach the wire. Rearming planned startup clears old
filter history. Production regressions cover integer/float stereo, the ten silent
priming AUs and continuity across the primed flag. Both old behaviors fail the
new checks. There is no new fade or filter arithmetic, and the deliberate
Suspend/Start repair pause is unchanged. The following hardware comparison tested
this candidate without further microphone transition changes.

## 0.1.25-r1 Windows S16 and float results, 28 October 2026

The source passed the existing 47 C sanitizer checks and 55 Python tests (one
existing optional skip). Both firmware variants passed ARM builds, descriptor
checks and linked-memory gates before flashing the float-capable build. Its measured codec allocation peak
leaves 75,392 bytes of the 263,432-byte linked heap, above the required 65,536-byte
reserve. These checks do not establish sustained hardware headroom.

The S16 microphone trial still performed one unplanned opening resync about
2.62 seconds after mic open, discarding 2,455 PCM frames and 2,976 frames of
scheduling debt. The following approximately 14-second interval reached
99.979 AU/s without further source loss. A separate planned Suspend/Start repair
followed mic close. The listener reported substantially improved transitions and
accepted the result as good enough; no continuing one-ear failure was reported.

Windows was then changed from S16 to 48 kHz stereo IEEE float32 using its built-in
`usbaudio2` driver. The endpoint device format read back as float32 with 8-byte
frames and 384,000 bytes/s, and Pico diagnostics reported speaker alt 2 throughout
playback and the microphone trial. This confirms actual float USB transport,
independently of the Windows mix format. Endpoint volume and mute were unchanged;
the original device format was saved locally for rollback. NTPods remained closed.

The initial float music window lasted 11.042 seconds at 99.9805 AU/s, with no new
active discarded/zero-filled frames, recoveries, controller flushes, malformed
USB packets or short reads. The listener confirmed clean sound in both ears.
During the 16.992-second observed mic interval, one opening resync discarded 2,411
PCM frames; the average including that event was 96.046 AU/s. There was no active
zero-fill, controller flush or USB error. The overall recovery count rose twice:
one unplanned opening resync and the separate planned closing repair. Float alt 2
remained selected. Game Mode was off by listener choice for both comparisons.

The listener rated the float mic test the same or better than the accepted S16
test, with normal audio in both ears afterward. Keep the accepted transition
behavior; this does not remove the measured opening resync or deliberate closing
pause. No full 30-minute acceptance interval, current Android normal playback or
Symfonium exclusive result has been recorded for this build.

Local evidence: `float-playback-result-0.1.25-r1.json`,
`mic-float-result-0.1.25-r1.json`, `mic-open-s16-0.1.25-r1.*`,
`windows-float-migration-0.1.25.log`, `windows-formats-after-float-0.1.25.txt`
and the matching capture files in the private bring-up output directory.

## 0.1.33 Pixel USB baseline, 28 October 2026

An initial listening check reported occasional USB hitches. The follow-up
read-only capture sampled once per second for 299.45 seconds, with S16 alt 1,
Game Mode confirmed on at 60 ms, and no Bluetooth phone input or microphone.
The listener reported no hitches during this longer interval.

The source produced 29,944 AAC-ELD units, averaging 99.9967 units/s, with two
units per outgoing packet. No new active sample discard, zero-fill, scheduling
debt discard, recovery, send failure, controller flush, malformed USB packet or
short read occurred. The 42 ms send-wait, 49.876 ms send-gap and 47.240 ms
packet-age maxima all predated the interval; they are not new stalls in it.
This supports clean source pacing for this interval, without establishing RF
delivery or completing the required 30-minute acceptance.

A separate volume issue was reproduced: a stem swipe changed the AirPods gain
and the host's ALSA hardware readback, while the Pixel's media-volume index
stayed fixed. The phone volume button changed both the index and USB hardware
volume. USB AudioControl readback alone therefore does not provide Android UI
synchronization. Keep audio buffering, clock adaptation and packet pacing
unchanged while correcting the control path.

## 0.1.38 PC hitch investigation and 0.1.39 candidate

After successful two-dongle handoff and short stereo/volume checks, the listener
reported PC hitches. Removing the standby dongle made no clear difference.
NTPods' native Bluetooth state was disconnected, and fixed volume counters
excluded ongoing gain writes during the observed steady playback. Closing the
companion helped somewhat by the listener's report, but did not remove the issue.

The companion-closed 31.27-second float-alt-2 sample received 1,500,912 USB frames
and encoded 3,126 AAC-ELD units. It added no active discard, zero-fill, short read,
bad packet, scheduling-debt discard, recovery or controller flush. One recovery
and its discarded samples predated that interval; lifetime counters were not
zero. A simultaneous later Windows loopback sample had no non-startup
discontinuity, timestamp error, position gap, zero or nonfinite sample. These
observations do not prove deadline delivery over the air.

Game Mode off was clean; switching it back on also remained clean. The off/on
retune cleared the current state, so this does not establish that the 60 ms
target itself is unreliable. Both mode captures kept continuous source cadence.
The displayed 92/69/165 ms reports were pre-adjustment estimates; they are not
measurements of the final receiver buffer.

Production ordering exposes a missed adjustment: ownership gating leaves the
transport stopped while Game Mode obtains its initial fresh delay report. That
report permits Start, but USB's live adjustment is unavailable until the 100 ms
prime has been sent. Once priming completes, the consumed report and confirmed
mode prevent a new adjustment. Candidate 0.1.39 adds a bounded post-priming USB
verification through the existing send/report/live-adjustment path. Its hardware
effect on hitches remains to be tested; codec, bitrate, packet grouping and target
latency are unchanged.
Game Mode now starts off by user request. A supported owned output receives a
fresh normal-buffer preparation, so a prior low-buffer setting is not mistaken
for the new default merely because the preference flag is off.

The original PC Pico was then flashed with 0.1.39, with firmware readback and
unchanged pairing storage verified. Before any USB frames or encoded audio,
its boot state reported Game Mode requested false, phase OFF and a fresh
150 ms normal preparation. The listener confirmed clean stereo in normal mode,
then enabled Game Mode by double press and confirmed automatic, clean stereo
after a closed-case return, with the companion closed.

The case-return observer recorded a 60 ms preparation, Stream Started, another
60 ms request, and a completed live correction. The post-prime report was 59 ms;
the 48-frame fill correction completed in 97 ms without failure. The later
snapshot retained float alt 2, Game Mode confirmed, and zero active discard,
zero-fill, debt discard, controller flush or recovery since boot. Startup drops
during connection/case absence were nonzero and must not be called lossless
playback. This short check establishes automatic correction and clean observed
playback; it does not complete a 30-minute run or measure final acoustic latency.

Local evidence: `pc-hitch-ipc-20261028-182002.json`,
`pc-hitch-hid-20261028-182427.json`, `pc-hitch-hid-20261028-182745.json`,
`pc-hitch-hid-20261028-183546.json`, and
`pc-hitch-loopback-game-on.log` in the bring-up artifact directory. The 0.1.39
checks add `pc-0.1.39-boot-state.json`, `pc-0.1.39-normal-playing.json`,
`pc-0.1.39-game-case-return.json`, and the matching `pc-startup-0.1.39-*.log`.

The second Pico was then updated over the Pixel's root ADB connection using
guarded remote BOOTSEL. Full-flash backup, verified image loading and unchanged
pairing storage were checked before reboot. Its idle snapshot reported Game
Mode OFF, no encoded audio and unresolved/passive ownership. After a manual
single-press takeover, the listener confirmed clean stereo through the custom
USB cable. The playback snapshot showed normal mode, a fresh 150 ms report,
USB S16 alt 1 and confirmed ownership. Active PCM discard/zero-fill, underflows,
send failures and recoveries were zero; startup drops and 18 controller-flush
events were nonzero. These are short-run observations, not extended acceptance
or proof of uninterrupted radio delivery. Evidence is in
`phone-0.1.39-flash-receipt.json`, `phone-0.1.39-boot-state.jsonl`,
`phone-0.1.39-boot-routing.log`, `phone-0.1.39-normal-playing.jsonl` and
`phone-0.1.39-normal-routing.log` in the bring-up artifact directory.
