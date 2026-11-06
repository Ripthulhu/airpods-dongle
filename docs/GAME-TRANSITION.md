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
clock changes. An independent 9/11 ms minimum preparation cadence prevents
inherited credit from sending faster than the requested receiver clock.
Cancellation and reversal retain the finite, actual-sent-frame drift target.

The final unit blends the carried waveform back to the exact normal fractional
FIR timeline. A splice is not mathematically transparent for all signals: a
short bass-envelope change can remain, although it avoids the former 10% pitch
bend. Do not describe synthetic pitch checks as listening acceptance.

Delay corrections above 500 ms are rejected before changing the sample clock,
matching Android's admission bound. The existing ten-second safeguard remains.

## Validation

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

Hardware music, both transitions, rapid toggle, pause/resume, volume and weak
signal checks are required before calling the preview accepted. Exact source,
test, binary, benchmark and deployment receipts belong with the release.

Algorithm background: [Verhelst and Roelands, WSOLA (1993)](https://sps.ewi.tudelft.nl/Education/courses/ee4c03/assignments/audio_lpc/roelands93icassp.pdf).
