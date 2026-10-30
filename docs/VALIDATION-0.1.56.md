# 0.1.56 relay and connection recovery candidate

## Measured 0.1.55 baseline

The Pixel sent LHDC v5, 48 kHz stereo, with AAC-ELD output and Game Mode off.
USB audio was absent, confirming the Bluetooth relay route. The listener heard
repeated hitches and glitches. Over 116.845 seconds, input decoded at 48,014
frames/s without new malformed packets, sequence gaps, submission errors,
decoder errors or buffer resets. There were 464,018 dropped PCM frames, fifteen
receiver recoveries and seventeen selected-AirPods controller flush events.
Intervals without recovery encoded a median 99.41 ten-millisecond units/s;
the lower overall rate does not establish CPU overload.

A same-connection BLE scanning comparison did not cure playback. Scanning paused
for a measured 34.5 seconds produced 72,896 dropped frames and two recoveries;
the restored interval of 34.39 seconds produced 89,359 and three. The listener
heard glitches in both. Scanning was explicitly restored, and this candidate
does not change BLE policy. The trace observed up to 93 ms send wait, 108.07 ms
send gap and 16.572 ms total encode-unit work. Lifetime maxima and correlated
flushes do not establish which event initiated every audible hitch.

## Corrections

- A failed outgoing media-channel open, rejected startup negotiation, or media
  Close no longer leaves a live signaling connection that cannot carry audio.
  Existing teardown and reconnect handle the current peer only; stale, malformed,
  wrong-endpoint and non-initiator events are excluded. Normal Suspend is retained.
- Relay output loss remembers only the authenticated phone session that was
  actually playing. When the AirPods return with local ownership, the existing
  bounded profile-reconnect machinery retries without sending Play or volume.
  Configured, open media is required for completion. Partial setup has a bounded
  timeout and cannot be revived by late events. USB, microphone, call, takeover,
  phone-session change and explicit yield cancel this recovery.
- Short transport stalls can be repaid using retained phone PCM instead of
  discarding healthy queued deadlines. The previous common deadline floor was
  2,880 frames (60 ms), even though the initial four-unit calculation was 40 ms
  for AAC-ELD. Phone music retains catch-up work while genuine input loss, FIFO
  overflow, receiver-reserve depletion and transport timeout remain protected.
  USB and call debt bounds are retained.
- A successful Pair request now retires the cleared peer's RAM identity and
  requests disconnection of only its existing authenticated ACL. Keeping that
  live ACL after deleting its stored key could let a profile reconnect work
  temporarily without producing a replacement bond for the next boot. Failed
  slot clears retain the original identity and key. The other AirPods slot and
  the phone ACL are unaffected, and authenticated discovery hints remain usable
  for a renamed pair without Device ID data.
- A genuine inquiry selection now arms reconnect intent before checking whether
  the old signaling connection is still closing. After that old connection
  releases, the normal retry connects the newly selected peer. An empty pairing
  target cannot re-arm the old peer, and a late old-CID release cannot cancel
  the replacement attempt.

The queue capacities, jitter and receiver targets, Game Mode settings, sample
width, codec quality, CBR bandwidth, volume and radio policy are unchanged.
Counter measurements and model timing do not establish acoustic latency.

## Validation state

Focused production regressions fail against the preceding implementation and
pass after the corrections under ASan/UBSan. They include wrong/stale peers,
failed media setup, rejected negotiation, media Close, case return, authenticated
session changes, partial replacement setup and retry limits. The phone scheduling
regression exercises the real recovery poll, with recoverable 65/80 ms waits,
genuine FIFO overflow, missing source data, clock wrap and USB/call guards.

The pairing regressions exercise both slots, successful and failed clears,
successful and failed inquiry starts, implicit phone-profile wakes, authenticated
ACL retirement and the old-signaling/new-selection race. Removing the retirement
call or restoring the previous connect ordering compiles but fails the intended
RAM-target or reconnect assertion. The corrected full HCI and playback-lifecycle
suites pass ASan/UBSan. No key material is included in their evidence.

## R1 hardware result and pairing diagnosis

R1 was built and installed, but the listener still heard relay glitches. Its
50.142-second capture added 323,613 dropped PCM frames, ten receiver recoveries
and nine controller flushes, while input decoded at approximately 48,003 frames/s
with no new malformed packets, sequence gaps, submission failures, decode failures
or buffer resets. This is a failed listening result, not acceptance of the
deadline-retention correction.

The first R1 boot also had no selected AirPods. Metadata inspection of the exact
pre-update private flash backup found a valid slot record whose two addresses
were already empty. Firmware loading preserved the final 64 KiB byte for byte;
the update did not erase that record. The retained-RAM/ACL paths described above
explain how a cleared pairing could still work until a reboot. A fresh pairing
on R1 selected the actual AirPods and ignored the nearby Power Dynamics speaker.
The R3 changes close those stale-identity and live-ACL paths.

## Final R3 build and deployment

All eight build stages passed for the frozen R3 source: pinned SDK verification,
sanitized regressions, adaptive/real-codec checks, SDK volume checks, phone-codec
checks, LHDC safety checks, configuration and firmware linking. The linked-memory
gate leaves 66,056 bytes in its worst emulated simultaneous-codec case, exceeding
the required 65,536-byte reserve. This does not measure hardware latency or
establish listening quality.

| R3 artifact | SHA-256 |
| --- | --- |
| Frozen source archive | `3bce74f29e074a7954feff6f80407844dc499cb6843a721025ae1a9b40ccd793` |
| UF2 | `7b94ccfea120030376cfd93f9c0018fa403558c8ab9b21ccb8b72ebe13b95300` |
| Linked-memory report | `807155240374edb934acc97204e79cba63bc6525b3d420e74048ea611da4c610` |

The phone Pico received that exact UF2 with a full private backup, image readback
verification and an unchanged final 64 KiB pairing region before reboot. After
the R3 reboot, readback confirmed a selected AirPods peer, A2DP and AACP readiness,
and AAC-ELD output. This verifies saved-pair return across that update/reboot;
it does not yet validate a new pairing performed entirely on R3.

The physical R3 closed-case return result is pending. R3 relay listening,
extended stability, Game Mode, range recovery and phone-call acceptance remain
open. Neither the failed 0.1.55 baseline nor the failed R1 listening result is
acceptance of R3.

Evidence is in the parent workspace's `outputs/relay-stability-20261030`,
`outputs/outgoing-media-open-20261030`, `outputs/outgoing-startup-recovery-20261030`
and `outputs/relay-continuity-probe-20261030`. Pairing negative controls and
sanitizer results are in `outputs/phone-pico-pairing-20261030/retirement-checks.json`;
the R3 build and deployment receipts are
`build-receipt-pcm32-0.1.56-r3-bw20000.json` and `phone-flash-r3-receipt.json`
under the relay-stability output directory.
