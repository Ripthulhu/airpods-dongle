# Relay output deficit on 0.1.57-r1

The user reported that relay playback improved after installation, then became
bad again over time. The degraded state was captured while the same connection
continued playing. The user subsequently switched to USB audio; that route was
left alone.

## Observed over 49.951 seconds

| Counter | Change |
| --- | ---: |
| Decoded input stereo frames | 2,399,040 (48,027.9/s) |
| Encoded 10 ms AAC-ELD units | 3,532 (70.71/s; real time requires 100/s) |
| PCM frames discarded | 623,368 (25.98% of decoded input) |
| Output transport recoveries | 21 |
| New zero-filled PCM frames | 0 |
| Controller flushes | 1 |
| Input malformed/sequence/decode/submit errors | 0 |
| Input buffer resets/timestamp discontinuities | 0 |
| Allocated heap | 99,640 bytes throughout |
| CYW43 bus-full retries | 0 throughout |

The input was LHDC at 48 kHz; the output was AAC-ELD, with Game Mode off. Input
counts represent PCM offered to the ring before overflow, not samples heard.
The ring repeatedly reached its 8,192-frame capacity. Output encoding and
delivery did not keep pace, and the repeated recovery intervals compounded the
loss. Stable heap counters do not show a leak in this window.

These facts do not prove either CPU saturation or radio saturation. Sparse HCI
credit snapshots retained four to seven of eight free slots. The bus-full repair
was not exercised in this session, although its asynchronous scheduling also
differs from the earlier firmware. The subjective improvement therefore cannot
be attributed specifically to prevented full-ring packet losses.

Detailed HID diagnostics required stopping LibrePods temporarily to avoid
competing transactions. Playback and both Bluetooth links remained active;
LibrePods was reopened afterwards. CDC diagnostics were also saved before
stopping the app. No codec, volume or Game Mode command was sent. No microphone
or PCM recording was collected.

## Source review

Two independent Astra reviews found no stereo-count or unconditional timer cap.
ELD consumes 480 stereo frames per 10 ms and phone output usually batches two
units. Encoding may proceed again as soon as completed work and sample debt
allow it. The idle helper does not add a sleep after a busy encode pass.

The old `unit_us` label means maximum prepare-to-completion time, not the audio
duration of a codec unit. The ±488 ppm phone clock adjustment is far too small
to account for the observed loss. Maximum encode and send-wait times do not
measure the fraction of each interval spent encoding, awaiting transmission or
recovering.

The audio snapshot's bitrate field is the negotiated value. The existing radio
snapshot also exposes the real adaptive current/target/ceiling values, but the
first capture helper omitted that portion. Its next revision preserves those
fields; no fixed encoder bitrate is inferred from this capture.

## Next diagnostic candidate

The offline r3 candidate adds cumulative wall-time counters to a separate `w`
request so the response fits the existing 2 KiB CDC ring. There is no new
per-packet logging and no change to audio scheduling,
codec quality, gain, buffer targets or Game Mode. The next comparison needs
encoder time, whole-unit completion time, successful and cancelled send waits,
and recovery time over one stable connection. Main-loop counters include time
waiting for the CYW43 lock, previously excluded from Bluetooth work timing.
Durations include preemption; overlapping categories must not be added as
independent CPU utilization figures. Totals wrap modulo 32 bits and are compared
with unsigned subtraction over short intervals.

`aac_calls/aac_us` count actual encoder calls, including failed calls;
`aac_jobs/aac_job_us` include completed prepared jobs even when invalidated.
Sent samples count successful host submissions, not confirmed receiver delivery.
Cancelled waits are measured before their packet state is cleared. Recovery
time is sampled at playback polls, so each transition has up to one polling
interval of boundary uncertainty. `[OUTPUT_RATE]` reports the actual adaptive
encoder state. The report does not reset counters.

This is instrumentation, not a proven audio fix. The phone installation is
recorded below; no relay listening improvement is claimed.
The evidence lives in the parent workspace under
`outputs/relay-driver-audit-20261030/live-degraded-*`; `summarize-live.py`
reproduces the interval totals from the six saved snapshots.

## Diagnostic build validation

The frozen r3 build passed all nine stages: SDK checks, sanitizer regressions,
adaptive codec, CYW43 transport, volume, phone codecs, LHDC safety, configuration
and firmware linking. The final heap reserve after the worst tested codec peak
is 65,912 bytes, above the required 65,536. Downloaded release hashes and UF2
boundaries were verified without accessing a dongle.

- Frozen source SHA-256: `fb551d5767cd36f444bafd0402952f784cea04d669c8f25a20012557a01ab5bf`.
- UF2 SHA-256: `13f3afbd7f276ebc0aeb59a66d0a22f909135205f6dacd8a1695c3ab2ced8829`.

Documentation was updated after the frozen source. Later transport test-only
additions also passed against the same pinned, patched SDK; production code in
the r3 artifact was not changed.

## Phone installation

At the user's later request, r3 was installed on phone Pico `DC1F379C3B1EFCA5`.
The exact UF2 was verified by picotool after loading, a full 4 MiB private backup
was retained on the Pixel, and the final 64 KiB pairing region was verified
byte-identical before reboot. The runtime reports all three new `w` lines and
reconnected on AAC-ELD with USB audio available. The PC Pico was not changed.

The Pixel player was paused before the update. Preflash physical gain was 56/127;
after re-enumeration it read 127/127. This is a separately observed restart
volume issue, not evidence about relay timing. With playback paused and the
companion stopped to avoid competing writes, a generation-fenced host-authority
transaction restored exactly 56, confirmed settled readback, and released its
lease before LibrePods was reopened. No codec or Game Mode setting was changed.
The firmware candidate itself has no volume changes. A pre-authority USB volume
initialization is a possible cause of the reset, not a confirmed diagnosis.

Installation and runtime receipts are in
`outputs/relay-driver-audit-20261030/phone-flash-r3-receipt.json` and
`phone-r3-postflash-*.txt`. No post-install relay listening comparison has run.

## Live follow-up on r3

The user subsequently reported substantially improved playback, with occasional
glitches, while playing a video through LHDC v5 relay. This is a listening
observation, not evidence that the diagnostic-only r3 changes fixed audio.

A 120.173-second CDC sample with LibrePods running averaged 99.973 AAC-ELD
units/s and 47,986.8 submitted stereo frames/s. Current encoder bitrate stayed
255,200 bit/s and transport recoveries did not increase. Encoder elapsed time
was 65.55 seconds, whole-job time 85.19 seconds, and successful send waits
2.08 seconds; these overlap and must not be added as CPU utilization.

During a subsequent detailed counter capture, the interval from device uptime
1,028,594 to 1,031,286 ms contained 3,584 newly zero-filled frames (74.67 ms) and
2,379 discarded frames (49.56 ms). There were no new input sequence, malformed,
submission, decoding or timestamp errors; no input buffer reset, output
recovery or controller flush occurred. This establishes fresh PCM starvation
and overflow while output still ran at real time. Sequential RPC snapshots do
not establish the ordering or exact origin of the input delay.

A later 39.283-second capture was clean: 3,929 encoded units, no new loss,
recovery or flush; sampled PCM fill 3,180–6,086 frames, at most two queued input
packets, and ready decoder output in one of 41 samples. This does not establish
sustained decoder saturation. The trace accumulated 1,200 events, with all 975
retained unique events being timer lateness (type 3), up to 7 ms. Type 2 denotes
zero fill. Frequent lateness events overwrote the context around the earlier
glitch, so persistent loss-specific context is the next useful measurement.

The eight-slot input queue and single decoded-output mailbox can extend burst
catch-up through main-loop handoff latency, but no synchronization correctness
bug was established. A second output buffer would be phone-only dynamic memory,
not a reduction of the USB microphone mode's remaining reserve. Neither that
change nor a larger playout target is justified by the current capture.

No route, codec, gain, buffer target or Game Mode write was sent. LibrePods was
briefly closed for exclusive HID reads and reopened; the player was not paused.
Final checks still show playback active, the same Bluetooth handles and physical
gain 56/127. Captures, `summarize-r3-live.py`, and `live-r3-summary.json` are in
`outputs/relay-driver-audit-20261030` in the parent workspace.
