# USB audio continuity and float transport

## Acceptance status, 28 October 2026

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

`AIRPODS_USB_FLOAT=OFF` builds the same processing with only alt 1 exposed. Use
this comparison artifact to establish a clean S16 baseline before enabling float
USB on hardware. The normal default is ON, retaining alt 1 compatibility.

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
