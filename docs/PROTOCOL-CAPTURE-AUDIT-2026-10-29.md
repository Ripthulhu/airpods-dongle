# Protocol comparison with saved macOS captures

This review compares the Pico implementation with the saved Tahoe/Pro 3 captures.
An observed difference does not establish that it causes an audible failure.
Raw captures, hardware identifiers and private pairing records remain outside Git.

## Corrections in candidate 0.1.47

- **Game Mode controller timeout:** macOS changes automatic flush timeout from
  200 to 60 ms and restores it afterward. The Pico previously retained 200 ms.
  The new policy follows the captured values on the selected AirPods ACL and
  verifies controller completion. Evidence: `20261014T210429Z-75985976`, command
  and completion frames 2726/2735 and 6938/6949; `working-control-frames.txt` and
  `working-media-validation.json`.
- **Volume precision and report ordering:** hardware reproduction showed Windows'
  finer USB request being replaced by its rounded AVRCP equivalent. A separate
  production-handler test reproduced an old INTERIM overwriting a newer SET ACK.
  Both are corrected and regression-tested. See [validation](VALIDATION-0.1.47.md).

## Missing setup and feedback messages

The Pico sends the handshake, its supported-feature envelope, notifications and
key request. Compared with macOS/NTPods it omits:

| Message | Evidence | What remains unknown |
| --- | --- | --- |
| Source announcement `04 00 04 00 24 00 02 00 01 00 10` | Native setup inventory, frame 5576 | Effect on receiver behavior and safe retry/order on Pico |
| Capability request `04 00 04 00 01 00 08` | Native setup inventory and NTPods initialization | Required use of its reply for model-specific controls |
| Manual volume feedback `04 00 04 00 4C 00 05 00 03 01 old new 00` | `20261015T190602Z-2cc0a450`, frames 6773 and 6892 | Remaining fields and whether it affects tones or synchronization |

The manual-volume run used modified SDP attributes and lacked independently
marked gestures. It establishes a wire difference, not a ready-made volume fix.
The native handler calls it feedback; it is not a command enabling stem control.
See [volume evidence](VOLUME-EVIDENCE.md).

The Pico's `0x4D` mask is `0x00FF`; the saved native envelope includes `0xF2FF`.
Native binary analysis associates bit `0x0200` with hearing support. Copying the
whole mask would advertise features the firmware may not implement. The source
announcement, capability discovery and each support bit require separate checks.

Game Mode eligibility also uses a model/firmware heuristic rather than the
captured remote SDP A001 low-half `0x100000` support bit. This is a compatibility
gap for other models, not evidence explaining the current Pro 3 hitches.

## Verified matches and unresolved differences

All 6,887 Mac access units in the successful Game Mode capture decoded with the
firmware's plain stereo ASC `f8e65000`: AOT 39, 48 kHz, 480 samples, no SBR.
Four-byte AU headers and three-frame normal/two-frame Game Mode grouping match.
Evidence: the capture's `eld-offline-check/findings.md` and `results.json`.

The native Game Mode packet interval averaged 19.968 ms with a 21.808-ms maximum.
The Pico's submission timing is more variable. This warrants delivery-timing
measurement; clean PCM counters alone do not establish timely radio delivery.

Mac RTP uses elapsed milliseconds and SSRC zero. The Pico retains its tested
sample clock. Previous Windows playback of exact captured AUs with millisecond
timestamps still suspended after 273 packets; the sample clock played 5,472
packets clearly. Clock, SDP identity and session behavior must be tested together
before replacing the clock. Minimal Apple DID also differs from Mac A000/A001/
AFFF metadata; no validated complete replacement mask or clock selector exists.

Apple's captured variable-rate frames averaged about 227 kbps; Pico fixed-rate
256-kbps output is a supported mode difference. It changes radio load without
establishing malformed packets. A bitrate comparison needs its own saved-state
test rather than an unannounced quality reduction.

Saved Mac microphone traffic is HFP/CVSD and contains no AACP `0x58` uplink. It
cannot validate a replacement high-quality microphone sequence. The Pico's
start/stop bytes match the existing high-quality protocol reference. The Mac
HFP restart resets RTP counters; applying that behavior to the distinct AACP
path remains unverified.

The 52-capture AVRCP inventory supports the current controller/target roles:
macOS subscribes to the buds' VolumeChanged event and does not advertise that
event in its own target list. No captured bud-to-Mac SetAbsoluteVolume or volume
passthrough command justifies changing roles or adding a target notification.

The remaining setup differences are documented for controlled comparisons.
Candidate 0.1.47 does not claim that they or its timeout correction have resolved
the reported acoustic hitches.
