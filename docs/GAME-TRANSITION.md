# USB Game Mode transitions

The current preview uses the ordinary USB sample-rate converter to change
playback speed briefly when switching Game Mode. The AirPods still receive
48 kHz stereo AAC-ELD in 480-frame packets. To fill the receiver buffer the
dongle sends those packets closer together and uses fewer USB source frames
per packet. To drain it, the dongle does the reverse. At the nominal 9 and
11 ms intervals a steady 1 kHz tone briefly becomes 900 or 1100 Hz. When the
adjustment finishes, the same filter resumes its normal rate with its sample
history and fractional position intact.

On entry the converter consumes 432 or 528 USB frames. Subsequent packets
smooth the difference between actual USB arrivals and the source frames
already consumed, keeping the rate between 384 and 528 input frames per
480 output frames. Real packet submissions determine when the adjustment has
finished. If a refill falls behind real time long enough to threaten the
receiver reserve, the dongle returns to normal pacing and reports the failure.
The actual delay reported by the AirPods and the previous silence trace are
documented below.

The pipeline regression starts with the 64 ms receiver reserve from the PC
capture. It counts only packets the simulated transport accepts and drains the
receiver at 48 kHz between arrivals. With a modeled 1 ms filter cost and
6.0–6.3 ms encoder cost, restoring the 150 ms target takes about 0.76 s.
The simulated receiver has 140–160 ms buffered at completion and never runs
out of samples. A separate 10.72 ms workload is too slow
to build a reserve. In that case the firmware abandons the refill early enough
to sustain normal playback in the fixture. Live radio scheduling and audible
continuity need a listening check on the candidate.

## Earlier pitch-preserving implementation

Version 0.1.61 replaces the live USB transition's 0.9/1.1 PCM sample-rate
conversion with bounded waveform-similarity overlap-add. The existing receiver
buffer commands, finite 9/11 ms packet pacing, RTP accounting, controller policy
and 48 kHz stereo AAC-ELD format remain in place. The first unit consumes
432/528 source frames per 480-frame encoded unit. Later units follow actual USB
arrivals, retaining odd-frame and clamped debt, so encoder load does not quietly
accumulate or discard source audio. Consumption is bounded to 384–528 frames;
the final return to normal consumes 480.

The former conversion demonstrably changed a 1 kHz source to 900/1100 Hz.
Android's native encoder changes packet pacing while reading unchanged samples
on demand; its buffering cannot be copied directly into this fixed-rate USB
pipeline.

### Implementation and ownership in the pitch-preserving builds

From 0.1.67, Game Mode preserves both the controller-confirmed packet mask and
the normal 200 ms controller flush timeout. It changes the receiver's 60/150 ms
target and finite audio clock correction, without adding 3-Mbit EDR exclusions.
Startup confirmation and outstanding controller-command restoration retain
their owner, epoch and late-reply fences. This policy change leaves the USB
transition and PCM processing below unchanged.

`playback_tempo.h` chooses shared stereo waveform offsets in 240-frame hops.
Its bounded search includes a 2400-frame lag span to cover deep bass. It uses
adjacent sample pairs rather than a mono sum, preserving opposite-polarity
stereo. Quantized, scaled samples are used only for correlation; selected audio
is copied again from the original float ring, filtered at the retained fractional
phase, and delivered to the existing Q31 encoder input. No S16 stage is added to
audible PCM. Normal playback continues through the existing resampler.

The correlation and selected-audio views share the previous resampler workspace.
There is no new heap allocation. Preparation snapshots correlation data under
the producer lock, searching runs outside both producer and Bluetooth locks,
and selection reacquires the producer lock briefly. Before consuming input,
the worker checks its encoder epoch, exact source/connection scope, read cursor,
fraction, PCM generation, retained history and required future input. Rendering
then runs unlocked. Only a current, successful encoder job commits the next
overlap state. A reset or overwritten/trimmed/zero-padded timeline invalidates
history. Source-time credit retains fractional and negative balances across
clock changes. From 0.1.70, a preparation deadline advances by 9/11 ms from the
scheduled time. Brief lateness can be recovered one unit at a time within four
intervals (at least 40 ms). A longer gap cancels a slow adjustment. A fast
adjustment resumes from a fresh deadline and still has to earn its lead from
actual submissions. A slow adjustment cannot finish on a send that is at least
one interval behind its next preparation deadline.
Cancellation and reversal retain the finite, actual-sent-frame drift target.

Direct USB AAC-ELD sends one 480-sample access unit per RTP packet in both modes,
matching LibrePods' native encoder. The phone relay and call routes keep their
grouped packets. More frequent USB-path sends need live controller and weak-signal
checks, including packet gaps and completed packets.

Both 0.1.69 and the first 0.1.70 preview reset the RTP timestamp and AAC-ELD
access-unit sequence when reopening the encoder. Bluetooth media recovery can
suspend and resume on the same channel, where BTstack keeps the RTP sequence
number. The November 23 preview trace showed the RTP timestamp jump backward
across such a recovery while the connection stayed up. LibrePods preserves both
media counters on a feeding reset. The dongle now initializes them when a new
media channel opens and keeps them through encoder recovery. AirPods playback
after a live recovery still needs verification.

The final unit blends the carried waveform back to the exact normal fractional
FIR timeline. A splice is not mathematically transparent for all signals: a
short bass-envelope change can remain, although it avoids the former 10% pitch
bend. Do not describe synthetic pitch checks as listening acceptance.

Delay corrections above 500 ms are rejected before changing the sample clock,
matching Android's admission bound. The existing ten-second safeguard remains.

## USB clock feedback feasibility

The current USB speaker advertises an adaptive OUT endpoint with a fixed 48 kHz
format. Its 1 ms PCM32 packet is capped at 392 bytes, or 49 stereo frames. An
asynchronous OUT endpoint with explicit feedback could request variable packet
sizes. One 480-frame unit every 11 ms needs an average of 43.64 frames/ms. One
every 9 ms needs 53.33 frames/ms and a maximum of at least 54 frames (432 bytes
for stereo PCM32). Feedback alone cannot exceed the current descriptor limit.

USB Audio 2.0 Type I allows only a one-frame deviation from nominal at a fixed
sample rate. The pinned TinyUSB feedback helpers follow that rule and limit 48 kHz
full-speed playback to 47–49 frames/ms. A wider endpoint and raw feedback can be
used for a lab experiment, but those Game Mode rates are outside the fixed-rate
packet rules. See section 2.3.1.1 of the [USB Audio 2.0 data formats specification](https://www.usb.org/document-library/audio-devices-rev-20-and-adopters-agreement).

Windows supports explicit feedback on an asynchronous OUT alternate setting.
The Android common kernel uses the feedback rate to size playback packets and
caps them at the negotiated endpoint maximum. These capabilities do not establish
how either host's mixer behaves during a brief 9–11% rate offset. A host that
queues unchanged PCM can absorb the rate mismatch upstream, moving the latency.
If a host resamples the source instead, pitch and timing need to be measured.
[Windows USB Audio driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers),
[Android common USB audio driver](https://android.googlesource.com/kernel/common/+/refs/heads/android13-5.15/sound/usb/endpoint.c),
[TinyUSB feedback reference](https://docs.tinyusb.org/en/latest/reference/class/audio.html).

An isolated feedback build can validate enumeration, PCM32, volume and the
microphone on each host, then measure actual samples/ms, host and device queue
depth, pitch and end-to-end latency during small clock corrections. Keep the
USB clock fixed when comparing the two transition algorithms.

## Earlier validation

The November 16 PC report found crackle and a short interruption in both
directions despite clean steady Game Mode playback. One repeated On/Off check
added 432 active dropped frames, 3,010 discarded source-debt frames and one
timeline recovery, with no USB packet errors or controller flushes. A separate
worst-cost model (7.93 ms AAC plus 3.30 ms tempo on every unit, not an observed
average) reproduces recovery when restoring the normal receiver target.

The 0.1.68 candidate sums each fine-search reference energy once and
filters four output frames together without changing each frame's tap order.
It retains the selected waveform, PCM precision, receiver targets, clock policy
and existing static workspace. A scalar pre-change oracle checks identical Q31
output, selected lags and history across 600 broadband, quiet, silent and
fractional-phase entry/exit/reversal jobs. On the same PC Pico at 250 MHz, the
RAM-only four-profile benchmark reduced mean fractional-phase processing from
3.277/3.306 ms to 2.943/2.964 ms (9/11 ms intervals). Whole flash was compared
before and after both runs and remained unchanged; 0.1.67 was restored. These
synthetic timings exclude the encoder, radio and normal interrupts. Listening
acceptance remains pending; arithmetic equivalence and a lower CPU cost do not
prove sufficient throughput for every signal or host.

That first listening repeat still failed, adding 314 active dropped frames,
88 zero-filled frames and two recoveries. The 0.1.69 candidate therefore splits
preparation into bounded chunks, releasing PCM exclusion between groups of 256
source positions so the USB timer can run. The previous preparation could mask
interrupts for about 637 microseconds; USB servicing is scheduled every 500.
The pinned USB device driver uses a single-buffered OUT endpoint, so a missed
host transaction before its callback need not increment bad-packet counters.

Every reacquisition validates the original encoder/source scope, cursor,
fraction, generation, history and required future input. Invalidated work cannot
consume replacement PCM or publish an encoder job. Post-read validation, clock
debit and job publication are atomic under one short exclusion. An unowned
route is rejected before reading PCM. These changes preserve the waveform math,
existing static workspace, receiver targets, clock policy and volume handling.
Real interrupt timing, USB-frame conservation and listening acceptance remain
pending; host checkpoint tests establish service opportunities and fencing.

`tools/test.py --sanitize` includes actual-production-path waveform tests for
unequal and opposite-polarity stereo, deep bass through treble, low-level float
precision, transitions, reversal/cancellation, retained-history races and USB
buffer accounting. Clock/stall and volume regressions remain in the same runner.
The ARM build must also pass the linked codec/PCM32/.0076 identity and 64 KiB
runtime-reserve gates. Resource limits must not be lowered to admit this change.

CPU timing is measured separately on a Pico 2 W at the production 250 MHz clock
using an SRAM-only, synthetic-input benchmark. Its result must be combined with
encoder and scheduler cost; it does not establish radio delivery or acoustic
latency. The working flash image and pairing storage are compared before/after
the test and the board returns to its existing flash firmware. The frozen DSP
header measured at most 660 µs preparation, 1265 µs search, 140 µs selection and
1231 µs fractional rendering across four 100-unit profiles: a conservative
3.296 ms sum. The producer lock covers preparation and selection separately;
search and rendering run unlocked. These are synthetic timings, not a bound on
all interrupts or radio scheduling. Production-path tests also model 3.37 ms
DSP plus 6.0–6.3 ms encoder work, USB ticks, a 40 ms stall and an insufficient
throughput case that must fail finitely rather than claim a completed buffer fill.

Code `52a0bf8` passed the PC listening check on 2026-11-17: paused-phone button
takeover and both Game Mode transitions sounded clean, with no menu errors.
A 22.31-second switching window retained ownership/session/generation/PCM32,
with active drop/zero, USB errors, discarded debt, recoveries and flushes all zero.
Normal Off and confirmed On are observed; the later Off request is captured
restoring and its settled menu result is user-reported. The startup-drop count
persists, so the zero active counters are not a diagnostic reset. Later manual
reconnects are outside that window. Acoustic latency was not measured.

Hardware music, both transitions, rapid toggle, pause/resume, volume and weak
signal checks are required before calling the preview accepted. Exact source,
test, binary, benchmark and deployment receipts belong with the release.

On November 23, the 0.1.70 recovery preview stayed silent with the same
Bluetooth owner and advancing RTP timestamps. In 13.985 s, Windows supplied
671,136 PCM frames and the controller reported 1,398 packet completions, with
no recorded PCM losses or controller flushes. Controller completions cannot show
whether the AirPods rendered the audio. Pausing and resuming restored sound
without replacing the connection.

During silence, the last delay report was zero. The Off request could not
confirm normal buffering. A later Off retry during audible playback transmitted
150 ms and received a fresh 110 ms report, initiating a 9 ms clock adjustment.
The firmware kept the earlier failure latched even after a fresh Off report.
The explicit Off path now clears the old failure on fresh confirmation. A failure
in the current clock adjustment can still surface. This addresses status and
restoration bookkeeping. The cause of the silent AirPods stream remains open.

## PC Game Mode Off investigation, November 23

The motion-capable 0.1.70 preview reproduced silent playback with the motion
reader closed. The dongle sent the 60 ms request, received a fresh delay report,
and completed its entry clock adjustment in 824 ms. On exit it sent the 150 ms
request, received a fresh 64 ms delay report, and began the 9 ms refill.
It sent 447,840 stereo frames in 10.003 seconds, equivalent to 9.33 seconds of
audio at 48 kHz. The receiver needed an 86 ms gain. The sender instead fell
673 ms behind real time and reached its ten-second timeout.

Across the capture the counters for lost or zero-filled active USB frames,
failed sends, controller flushes and stream recoveries stayed flat.
After the listener reported silence, controller completions continued at about
100 per second. The receiver buffer draining fits these observations. Its
instantaneous fill and the precise time it stopped rendering are unknown.
Pausing and resuming the player failed to recover sound. Reconnecting the
same AirPods restored audible stereo playback with Game Mode off.

The first unflashed recovery candidate checked the actual sent-frame deficit
on each fast restore packet. The variable-speed candidate retains this guard.
Four consecutive sends more than half the reported starting delay behind real
time end the unsuccessful refill and return to normal pacing. At the measured
average cadence, a replay stops after 525 ms. The host pipeline also exercises
that workload with a simulated 64 ms sink and checks continuous output after
the fallback. A new Off request or an eligible fresh USB stream can retry
with a new receiver report. A separate regression exercises a short PCM window
that used to wrap a 48-frame deficit to 4,294,967,248 queued frames and force
a stream restart. Hardware acceptance still requires a confirmed live Off
transition and continuous audible playback.

The first unflashed candidate also optimized the waveform search. The current
preview removes that search entirely. Its 32-bit PCM converter reuses the
same filter coefficients across output frames with the same fractional phase.

Algorithm background: [Verhelst and Roelands, WSOLA (1993)](https://sps.ewi.tudelft.nl/Education/courses/ee4c03/assignments/audio_lpc/roelands93icassp.pdf).

## Hitches during steady USB playback

Three recorded USB bursts crossed the 3,840-frame backlog limit by 5, 22
and 94 stereo frames. Each triggered the same recovery as a prolonged
stall, including a Bluetooth stream restart. Active USB AAC-ELD now uses
up to one extra 480-frame access unit (10 ms) in the existing ring before
trimming. The normal queue target and the 60 ms Game Mode target stay the
same. Overwriting unread PCM or exhausting the extra room still triggers
recovery. The pipeline regression replays all three bursts and checks that
the sender delivers every frame without a restart.

During confirmed 60 ms USB Game Mode playback the bitrate controller also
checks how much time the sender spent waiting for Bluetooth. A 44 ms wait
followed by a 54.7 ms gap between submissions leaves less than one 10 ms
packet of headroom. Repeated, distinct waits like this can lower the bitrate
one step even when they straddle a one-second observation boundary. An
isolated wait still allows the stream to keep its current bitrate.

The audio trace records long gaps between controller credit returns on the
selected AirPods connection. Use the timestamps and batch sizes alongside
the sender trace and a listening check. A returned credit cannot establish
when either earbud played the audio. See [Audio continuity](AUDIO-CONTINUITY.md).

### Background case scanning

During a PC Game Mode listening test, pausing passive Bluetooth scanning
reduced controller credit gaps of 40 ms or longer from 295 to 17 in about
90 seconds. Restoring scanning brought the count back to 307. In a separate
three-minute run the listener reported clean playback until scanning resumed.

The dongle defers background scanning while Game Mode audio plays. Switching
Game Mode off mid-track keeps the scan deferred until playback pauses. Case
battery and Nearby Finder can receive fresh advertisements again during the
pause if the radio is available. A microphone session or device search can
independently keep the scan paused.

A closed USB audio stream releases the deferral immediately. If the host
keeps the stream open, one second of digital silence counts as a pause.
An equally long silent passage in a track has the same effect. The Bluetooth
relay uses its input stream's start and suspend events.

### Game Mode and A2DP scheduling priority

On the .0076 radio, Game Mode automatically requests high scheduling
priority for the selected, authenticated AirPods connection during USB
AAC-ELD playback. An eligible local or peer AVDTP Start can submit the
request before streaming starts. Enabling Game Mode during a stream can
also request it. Audio continues while the radio processes the command.
The request lasts for the eligible stream, including sessions longer
than five minutes.

Switching Game Mode off, suspending or closing the stream, or losing the
eligible route releases priority. Resuming eligible playback requests it
again. The Bluetooth stream can stay open while Windows sends silence,
so a paused player alone does not prove that AVDTP has suspended.
USB typed feature 25 exposes the most recent command result and transition
reason. [Feature layout](CONTROL-EXTENSIONS.md#a2dp-source-priority-usb-typed-feature-25)

The vendor command is `Write_A2DP_Connection` (`0xFD1A`). Its four
parameter bytes are the little-endian classic connection handle, priority
(`1` for A2DP, `0` for ordinary scheduling) and direction (`0` for
the dongle acting as the source). A successful Command Complete contains
only the opcode and status. Match it to the single outstanding request.
Record the confirmation for both enabling priority and restoring normal
scheduling. There is no independent readback of the scheduler in this test.
[Infineon guidance](https://community.infineon.com/t5/Knowledge-Base-Articles/Bluetooth-Scan-Operation-and-A2DP-Streaming-in-CYW43xxx-Devices/ta-p/735062)

In the three-minute PC listening test, the radio accepted high priority
before the fresh stream start. All 179 samples showed high priority and
255.2 kb/s AAC-ELD. The listener heard no hitches and the sender recorded
zero recoveries or radio flushes. We observed no credit-return gap of
60 ms or longer in 172.6 seconds of usable steady trace. The ordinary
reference also had zero marked hitches, with five such gaps and a different
receiver-reported delay (56 ms versus 45 ms). The high-priority capture
lost two trace events. These runs establish compatibility on the tested
link and leave the size of any audible benefit open.
