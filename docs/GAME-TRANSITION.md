# Pitch-preserving USB Game Mode transition

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

## Implementation and ownership

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

Windows supports explicit feedback on an asynchronous OUT alternate setting.
The Android common kernel uses the feedback rate to size playback packets and
caps them at the negotiated endpoint maximum. These capabilities do not establish
how either host's mixer behaves during a brief 9–11% rate offset. A host that
queues unchanged PCM can absorb the rate mismatch upstream, moving the latency.
If a host resamples the source instead, pitch and timing need to be measured.
[Windows USB Audio driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers),
[Android common USB audio driver](https://android.googlesource.com/kernel/common/+/refs/heads/android13-5.15/sound/usb/endpoint.c),
[TinyUSB feedback reference](https://docs.tinyusb.org/en/latest/reference/class/audio.html).

An isolated feedback build can first validate enumeration, PCM32, clock drift,
volume, and the microphone on each host at nominal rate. Then it can measure
actual samples/ms, host and device queue depth, pitch and end-to-end latency
through temporary rates and restoration. Keep the existing USB clock and
time-stretch path until that comparison demonstrates a benefit.

## Validation

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

Algorithm background: [Verhelst and Roelands, WSOLA (1993)](https://sps.ewi.tudelft.nl/Education/courses/ee4c03/assignments/audio_lpc/roelands93icassp.pdf).
