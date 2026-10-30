# 0.1.57 CYW43 transport candidate

The checked candidate is installed on the phone dongle as of 2026-10-30. The PC
dongle has not received it. The audible relay glitches remain unaccepted; a host
reproduction establishes a driver bug, not that it caused every hardware interruption.

## Confirmed packet loss

The pinned SDK's host-to-controller ring has 4,092 usable bytes. Its internal
write helper returns queue-full before writing when a packet cannot fit, but
the public wrapper discards that result and returns success. In Release builds,
the preceding assertion is disabled. BTstack consequently consumes an ACL credit,
advances fragmentation and releases a packet that never entered the ring.

An offline fixture compiling the actual SDK functions reproduces both lost
ACL data and a lost control command. With a deliberately stalled consumer,
eight 640-byte ACL payloads consume eight host credits while only six enter the
ring. This demonstrates that controller credits do not guarantee bus space.
It does not establish how often the real controller reaches this condition.

## Repair

The patched transport uses BTstack's asynchronous ownership contract. It retains
one packet in the stack's existing reserved buffer, reports busy while pending,
and retries a full bus through a one-millisecond timer. It returns to the event
loop between attempts so receive traffic can progress. Completion is emitted
once, after the shared-bus helper accepts the write. Pending state is cleared
before callbacks and cancelled on close. Unexpected non-full failures are fatal
rather than reported as successful transmission.

Queue-full is propagated through the bus wrappers without per-retry logging.
The patches are hash-checked against SDK 2.3.1 and its pinned driver submodule.
They add no audio packet queue. Codec bandwidth, sample format, volume behavior,
receiver targets and radio clock are unchanged.

## Measurements for the next hardware test

The existing explicit `t` diagnostic gains two lines; nothing is logged per audio
packet:

- `[BUS]`: accepted, committed, blocked, cancelled and fatal counts, plus current
  and maximum pending duration. Counts saturate and reset on transport open.
- `[PHONE_RATE]`: accepted compressed payload bytes, complete media bytes,
  packets and decoded frames, with time and codec. These cumulative counters
  wrap naturally; use unsigned differences over one stable connection.

Payload bitrate is `delta(payload_bytes) * 8000 / delta(ms)`. Media bytes include
RTP and codec headers but exclude L2CAP, HCI and radio overhead. A queue-full
count of zero during a reproduced hitch excludes this branch for that interval.
An accepted bus write is not proof of radio delivery.

The next comparison must correlate these counters with PCM drops, controller
completions and send waits while keeping the codec and volume fixed. Both USB
playback and relay playback require listening, pause/resume and case-return
checks because the shared transport serves both paths.

## Validation

All nine build stages passed: pinned SDK validation, sanitizer regressions,
adaptive AAC checks, the new production transport fixture, SDK volume checks,
phone codec checks, LHDC safety checks, configuration and firmware linking.
The transport fixture covers repeated full-to-ready transitions, timer progress,
actual BTstack fragmentation, control traffic, callback reentry, close/reopen,
wrapped writes and failure handling. Independent Astra review found no blocker.

The linked-memory check leaves 65,984 bytes after the worst emulated simultaneous
codec allocation, exceeding the required 65,536-byte reserve. This candidate uses
72 more bytes of static RAM than 0.1.56-r3. It does not measure acoustic latency.

| Artifact | SHA-256 |
| --- | --- |
| Frozen 0.1.57-r1 source | `d4be3b0805b75c111cdcee8df397a4d260e9f3bc0a0ba5ef007f285bc9227627` |
| UF2 | `8c254523ffcbdde19cfcaba4958b7e9128d25b1e6ebc1552ed339eb1778ecb77` |
| Linked-memory report | `27c069953c8569fc6ed3866b81882291efe68731f87efa1078ff9b12c16df369` |

The downloaded package was checked against the build receipt. All 764
non-document source files still match the frozen archive. Final validation prose
was updated after the build. During that offline investigation, no dongle was
flashed, no live codec or gain changed, and the shared SDK cache was left untouched.
Hardware testing was explicitly deferred at the user's request.

## Phone installation

The user subsequently requested installation and paused playback. The guarded
installer verified phone Pico serial `DC1F379C3B1EFCA5`, saved a full 4 MiB
pre-update backup privately on the Pixel, flashed the checked UF2, verified its
contents and version, and rebooted. The final 64 KiB pairing storage remained
byte-for-byte identical. The previous 0.1.56-r3 rollback image is staged beside
the new release on the phone.

After reboot, the dongle recovered its saved AirPods selection and connected
both Bluetooth links. Cached status reports AAC-ELD output; the paused phone
input reports LHDC. LibrePods reopened and reported its control connection
ready without an error. The new bus diagnostics respond with no fatal errors.
No codec or volume-setting command was sent during installation. Hardware
Volume remains requested; its fresh audio scope is unavailable while paused,
so gain restoration after playback is not yet verified.

The deployment receipt and bounded status/diagnostic snapshots are in
`outputs/relay-driver-audit-20261030/phone-flash-r1-receipt.json` and the adjacent
`phone-postflash-*` files in the parent workspace. The full flash backup remains
on the phone and is not exported. Relay listening, loaded bus counters,
pause/resume, case return and latency still need hardware validation. No claim
is made that the audible glitches are fixed merely because installation passed.

Relay PCM still passes through the existing S16 decoder output; the USB playback
path retains its wider PCM support. This transport repair does not change that
separate relay precision limitation.

The first live follow-up improved subjectively but deteriorated during sustained
relay playback. [The follow-up investigation](RELAY-OUTPUT-20261030.md) records
the measured output deficit and the diagnostic candidate prepared afterwards.

Evidence is collected in the parent workspace's
`outputs/relay-driver-audit-20261030`. The private pairing backups from earlier
work are not part of these artifacts.
