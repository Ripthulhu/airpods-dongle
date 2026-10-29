# AAC negotiation and Bluetooth servicing — 0.1.53

This candidate implements the two changes identified in the
[upstream comparison](UPSTREAM-AUDIO-COMPARISON-2026-10-29.md), at the user's
request. The previous 0.1.52 Game Mode run still hitched after adaptation reached
128 kbps, despite no measured active source loss. These changes address concrete
negotiation and scheduling weaknesses; listening acceptance remains separate.

## Negotiated permission and local encoding

An Apple peer's variable-rate permission is distinct from the selected local
CBR/VBR encoder algorithm. A capable peer now gets that permission with a
ceiling capped by both peers. A lower requested local target does not reduce
the negotiated allowance. Actual VBR requests still require peer support.
Received configurations must fit the advertised capabilities.

CBR can need one alignment byte beyond its nominal 10-ms access-unit size.
The local CBR ceiling reserves that byte: under a 256000-bits/s variable ceiling,
the maximum local CBR budget is 255200. Lower requests such as 220000 stay
unchanged when they fit. The requested setting and local encoder mode are
preserved; status reports the effective local budget. See the
[control contract](CONTROL-EXTENSIONS.md) and [AAC-ELD protocol](../aac-eld-apple.md).

A fixed-only peer retains a fixed CBR configuration and cannot trigger live
adaptation. If variable permission cannot accommodate 128000 CBR plus alignment
headroom, selection falls back to that fixed path. Unsupported actual VBR
combinations fail cleanly. No automatic CBR-to-VBR algorithm switch is added.

## Encoding outside the Bluetooth lock

The outgoing AAC encoder prepares one access unit under the CYW43 lock, uses
the existing PCM buffer and reserved output space while encoding outside it,
then completes under the lock. This lets Bluetooth background work run during
the expensive encode rather than waiting for the entire access unit.

The job pins its encoder handle. Close, reset and codec reconfiguration must
wait until it finishes. Only output from the current connection/source/USB
generation can be published, and send/fill paths cannot expose its unfinished
output. The scheduling debt for that unit is consumed once. No additional PCM
or encoded-packet queue is introduced; core 1 keeps its existing decoder work.

The host codec fixture guards actual steady-state AAC-ELD and AAC-LC encode calls
against malloc, calloc, realloc and free. It checks that each interception works
and that real encoder initialization is observed. Priming, configuration and
close remain outside the guarded encode interval and under lifecycle control.
Host allocator evidence does not replace on-device timing measurements.

The integrated simulation retains the existing 40-ms send-stall and receiver
continuity assertions. It now services production Bluetooth timer callbacks
during the unlocked encoder interval, and explicitly checks that those timers
remain blocked during locked work. Completing a full packet immediately requests
send availability; an extra empty loop pass must not delay every packet.

Review also covers cancellation with an already accumulated partial packet,
including USB reset, ownership and control-session changes. Discarded old audio
must not be combined with the next stream, and an old delivery deficit must not
carry across that boundary.

## Validation status

The frozen r2 source passed all eight build stages on 2026-10-29:

- 64 C regression fixtures under ASan/UBSan; 65 Python tests run, one optional skip.
- Real FDK: 15 AAC-ELD encode/decode cases and five AAC-LC allocator cases.
  The deliberate encoder-history-reset negative case was rejected.
- Pinned SDK volume tests, phone codec integration and LHDC safety checks.
- Standard float-capable Release firmware, descriptor validation and linked
  memory/codec-heap check. Speaker alternatives are 16-bit PCM and 32-bit IEEE
  float; the float endpoint accepts up to 392 bytes. Microphone format is unchanged.
- 65816 bytes remain at the emulated codec peak, above the unchanged 65536-byte
  runtime reserve. Static encoder-job storage is 116 bytes; total linked RAM
  growth is 128 bytes versus 0.1.52. This is a build/heap bound, not an on-device
  stack or listening result.

Source archive SHA256:
`4a03bc63c0eb999fd1d9eeb0dc340e3c9e19cc756d1583cc3f56b46fdec61770`.
The checked UF2 is 3632640 bytes, SHA256:
`79d4353ffbaff29c47f824d3a2f3540df3bf8fac7db805c4c3251218043c683d`.
The build receipt is `build-receipt-0.1.53-r2.json` in the lab project
`airpods-pico-bringup-20261027`; outputs are in `release-0.1.53-float-r2`.

The image and serial-pinned update guards are staged for both Picos. Neither
device has been updated to 0.1.53 at this checkpoint; readiness for a brief
playback interruption is pending. Keep the 0.1.52 rollback and compare the saved
pairing tail after flashing.

No 0.1.53 hardware acceptance result is recorded yet. Keep volume and radio
policy unchanged, compare normal/Game playback, then repeat the phone-case
outdoor test. Do not claim the hitches fixed from host tests or successful
negotiation alone.
