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

### Game Mode packet permissions

The working native Game transition also changes the selected link's permitted
ACL packet types. In `20261014T210429Z-75985976`, frame 2720 sends HCI opcode
`040F` with mask `EE1C`; Command Status 2725 succeeds and Packet Type Changed
2729 confirms it. On exit, frames 6934/6941/6944 restore `CC18` successfully.
Game entry is 14.538 ms after the AACP 60 ms target request, with confirmation
at 20.564 ms and the first two-AU packet at 22.691 ms. A second capture,
`20261014T204519Z-28986bc6`, repeats `EE1C` with successful confirmation at
frames 7177/7179/7183, while media is suspended.

The wire difference `2204` prohibits 3-DH1, 3-DH3 and 3-DH5; EDR packet-mask
bits have negative polarity. The SDK's positive internal EDR3 flags and wire
conversion (`XOR 3306`) confirm that interpretation. This permits 2-Mbit/s EDR
while retaining the other original permissions. It does not identify the actual
baseband packet chosen by the controller. See the Bluetooth Core
[HCI specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core_v6.3/out/en/host-controller-interface/host-controller-interface-functional-specification.html).

Through 0.1.49, the Pico application does not issue this per-link Game Mode
change. Its full-capability SDK default would produce `CC18`, but the current
Pico mask has not been measured; do not present that source-derived expectation
as a captured value. The next comparison should confirm the changed mask on the
selected AirPods handle while holding timeout, bitrate, packet grouping and
PCM policy fixed. Phone ACLs must retain their own policy.

The 0.1.49 timeout-only comparison was acoustically clean at both timeouts despite
one/five discard events in the two 60 ms intervals and none at 200 ms. It does
not establish a fix or eliminate this packet-policy difference. See
[the measured result](VALIDATION-0.1.49.md).

### Other setup messages

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
