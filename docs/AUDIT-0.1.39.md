# Renewed firmware audit: Bluetooth routing and interrupted audio

What this repo does: A Pico 2 W receives USB PCM or phone Bluetooth audio,
encodes a separate stream for AirPods, and bridges media controls and calls.
Expected load is one phone, one AirPods pair, optional microphone use, bounded
diagnostics and passive BLE scanning. This review covers source `30d8ce7` / docs
HEAD `e9cc909`, with production-code sanitizer reproductions and 0.1.39 captures.

## Must fix

1. **Android can silently switch the intended Bluetooth test back to USB.**
   - What: The same cable presents USB audio, controls and diagnostics.
   - Problem: Two purported Bluetooth listening captures actually had USB
     alternate 1 active and no sustained phone music packets. USB ownership then
     tears down the incoming phone audio profile.
   - Fix: Candidate 0.1.40 physically removed USB audio on the Pixel while CDC/HID
     worked, but media setup failure restored USB too soon. Candidate 0.1.41
     latches selection to the authenticated phone ACL session: media retries and
     profile loss cannot restore USB; full Bluetooth disconnect clears it.
     Existing USB microphone intent retains priority. Physical 0.1.41 checking
     on the Pixel passed: authenticated media selects control-only CAFE:4005,
     disabling Media audio retains that identity and no USB sound card while
     the phone ACL survives; full Bluetooth disconnect restores CAFE:4015 and
     the sound card. Reconnecting media removes it again. CDC/HID remain usable.
   - If skipped: The route changes underneath comparisons and users still get
     “problem connecting” while the cable supplies power/debugging.

2. **Phone output stalls cause substantial discontinuities.**
   - What: Decoded phone PCM is timed into the AirPods encoder and controller.
   - Problem: In `audit-0.1.39-pixel-bt-glitches.json`, a genuine Bluetooth-only
     4.371-second interval decoded 210,960 frames but discarded 41,376 frames
     (about 862 ms), with 45 new controller flush events. There were no new
     input sequence gaps, decoder errors or decoder resets. The sender produced
     348 encoding units, about 80/s against a 100/s target. The phone path also
     proactively trims at the shared 60 ms debt limit while PCM space remains.
   - Fix: Candidate 0.1.42 corrects the reproduced overflow feedback below and
     tracks cumulative sender shortage, using the existing confirmed restart if
     it exceeds the receiver reserve. In the newer 0.1.41 failure, 21.002 seconds
     elapsed while RTP advanced 19.980 seconds, yet the largest send gap was only
     49.34 ms and no restart occurred. Both links were central/master and there
     were no controller flushes. Final listening and the earlier radio-flush
     failure remain open; no larger buffer, overclock or scan change is assumed
     to solve them.
   - If skipped: Crackles, gaps and prolonged recovery remain possible, especially
     when an outgoing wait exceeds the receiver's rendering deadline.

3. **Physical PCM overflow leaves demand for samples that no longer exist.**
   - What: The incoming ring overwrites old samples when full.
   - Problem: The overwrite updates the loss counter but previously left encoder
     time debt unchanged. A reproduction with 8,000 buffered frames and 3,500
     owed frames pushes another 512: 320 are overwritten, then the existing debt
     clamp unnecessarily drops another 620 (940 total instead of 620).
   - Fix: Corrected in candidate 0.1.42. The initial 0.1.40/41 credit could cancel
     healthy future reads and keep an overflowing ring full. One early 960-frame
     packet followed by perfectly timed 20 ms packets reproduced zero encoded
     output for a second and 48,864 discarded frames. Credit now applies only
     to already-over-limit debt and expires immediately; the corrected replay
     encodes 48,000 frames with only the initial 1,344 overflow frames lost.
     Priming cannot carry old credits into active playback. Buffer sizes and
     clock policy are unchanged. Final hardware listening is still required.
   - If skipped: A transport stall can lead to additional avoidable sample loss
     or requests to encode audio that was already discarded.

4. **Healthy AACP reconnects exhaust a lifetime retry budget.**
   - What: The control channel can reopen while the media connection survives.
   - Problem: Three successful opens/closes still consume its retry budget;
     later loss can strand ownership and audio until a larger reconnect.
   - Fix: Candidate 0.1.42 resets the retry budget after a verified bidirectional
     control session. Incomplete handshakes retain bounded failure accounting;
     stale closes and explicit disconnects cannot revive a canceled session.
   - If skipped: Some later connection failures remain unrecoverable by control
     retry alone. This was reproduced in production code, not proven to initiate
     the steady-playback glitches above.

## Should fix

5. **AVRCP-only loss has no recovery while A2DP survives.**
   - What: Playback and media controls are separate Bluetooth channels.
   - Problem: A production harness advances 60 seconds after AVRCP loss without
     another connection attempt; stems/volume can stay broken while audio works.
   - Fix: Candidate 0.1.42 retries the current peer/media session up to three
     times after a channel loss or connect failure. An in-flight SDK setup is
     not repeatedly restarted; the SDK must complete or tear down a stuck SDP
     attempt before another safe attempt is possible.
   - If skipped: Recovery requires an unrelated reconnect.

6. **Takeover can lose the phone Pause, or toggle USB playback the wrong way.**
   - What: Switching ownership attempts to pause the old source.
   - Problem: The phone Pause is queued then cleared by disconnect before its
     sender runs. The separate automatic USB pause uses Play/Pause, which can
     resume an already-paused host. Both are reproduced using production code;
     host-specific idempotent Pause behavior still needs a physical check.
   - Fix: Open: bounded delivery of the high-priority phone Pause before teardown;
     an explicit automatic USB Pause, preserving intentional stem Play/Pause.
   - If skipped: The old source can continue or unexpectedly resume playback.

7. **SBC accepts a media payload too small for a frame.**
   - What: Codec setup accepts a positive outgoing payload budget.
   - Problem: A 100-byte payload cannot carry a 118-byte frame plus header; setup
     succeeds but no packet can progress. This is not the active AAC-ELD fault.
   - Fix: Open: reject or renegotiate when one encoded frame cannot fit.
   - If skipped: Some SBC negotiations start an unusable stream.

## Nice to have

8. **Usage instructions described obsolete defaults and button actions.**
   - What: README guides standalone use.
   - Problem: It said default-on Game Mode and single-press volume increase.
   - Fix: Corrected to default-off Game Mode and single-press takeover.
   - If skipped: Users cannot predict standalone startup or button behavior.

## Replay evidence and limits

The integrated diagnostic replay uses the actual worker, PCM/scheduling/packet
code, pinned SDK resampler, captured phone arrival timing and realistic CPU costs.
With a 76 ms outgoing wait every 500 ms, across three timer phases:

| Policy | Dropped frames | Inserted zeros |
|---|---|---|
| Existing 60 ms debt limit | 37,920 / 38,064 / 38,112 | 0 / 63 / 0 |
| Initial accounting candidate + 70 ms limit | 7,680 / 371 / 8,832 | 0 / 428 / 0 |
| Initial accounting candidate + 75–80 ms limit | 7,673 / 227 / 8,831 | 0 / 420 / 0 |

All were memory-safe under ASan/UBSan; none met clean-audio acceptance. Freezing
the phone clock during backlog removed zero-fill in this replay but increased
FIFO losses. Those threshold/clock changes are **not included** in 0.1.40.
The phone's 90 ms ingress reserve and 8,192-frame PCM capacity do not extend the
AirPods' 60 ms Game Mode or 150 ms normal rendering deadline.

The decode maximum is measured per incoming packet (typically 720 frames / 15 ms),
not per 240-frame codec unit; it cannot by itself establish CPU overload. The
existing BT-loop maximum excludes synchronous pending IRQ drain on lock release.
Controller flush events are real, but exact peer attribution and the initiating
radio/credit bottleneck remain unproven. The later USB-only captures cannot
answer that question. Diagnostic scripts closed their handles; continuous packet
logging was not enabled for the brief status checks.

Evidence is retained in project `airpods-pico-bringup-20261027`, including the
`audit-0.1.39/` replay logs and `audit39-runtime-credit-snapshots*.json`. The latter
must be labeled USB captures. Full 0.1.39 regression checks passed before edits;
candidate build/test receipts separately record 0.1.40 validation.

**Verdict:** Host routing is physically verified on the Pixel. Candidate 0.1.42
corrects reproduced scheduling and control-recovery faults, including feedback
introduced by the initial overflow accounting. A fresh authenticated phone Start
can also wake an already-owned peer-suspended output; passive connection remains
passive. The remaining Bluetooth delivery limits require listening validation;
there is no clean-audio release claim yet.

**Not checked:** physical listening on 0.1.42, USB personality switching on
Windows, full call audio, controlled RF/credit/codec comparisons, third-party
codec internals in their entirety, and end-to-end float32 listening after this
change. A silent Windows stream verified float32 USB input on 0.1.42; both boards
were flashed with their pairing storage unchanged. See the
[validation record](VALIDATION-0.1.42.md) for checks and remaining acceptance.
