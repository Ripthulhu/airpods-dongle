# Volume candidate 0.1.45 — 29 October 2026

The affected route reported by the user is Pixel to Pico over USB. Both physical
boards currently remain on 0.1.44; this candidate has not been flashed or heard.

## Reproduced faults

Production volume handling and the actual HID queue reproduce a delivered stem
change followed by pause before repair: physical 47 becomes 37, the software
host steps from 15 to 14, and the old resume code adopts physical 37 as its new
anchor. The fix preserves USB volume authority through ordinary PCM pause.
USB/session, peer, AACP ownership or phone-control transfer still retire it.

Another production regression queues an internal repair to 47, replaces the
AVRCP channel and observes fresh physical 20. The old code still sends 47 to the
new channel. The fix cancels this old internal repair while retaining a genuine
USB SET waiting for its first connection. Both isolated old-code negative
controls fail the new assertions.

The premature maximum cue follows a separate architectural problem: the old
relay restores 126 while Android independently attenuates software audio. A
native upward step can reach physical 127 below Android's UI maximum. Changing
the arbitrary anchor does not align those two gain authorities.

## Explicit host authority

Capability 128 and typed features 16/17 provide the bounded
[host volume contract](HOST-VOLUME.md). They expose fresh physical readback and
separate pending/in-flight requests. A five-second, USB-client/session/peer/CID
lease suppresses legacy HID volume steps, anchor repair, headroom adjustments
and UAC gain echo. Expiry/release cancels unsent owned writes and retains actual
gain; the remainder of that USB session stays quiet until a new lease rather
than assuming Android has restored its mixer policy.

Regression coverage includes software versus hardware hosts, paused writes,
initialization echoes, native stem changes, maximum/mute levels, malformed and
foreign requests, stale tokens and every lease scope change. A fresh INTERIM
before a later SET ACK is not replayed over the ACK. Rejected SETs retain real
readback, while a pending fresh readback is unknown in snapshots. Three isolated
old-behavior negative controls fail these ordering/freshness assertions.

This firmware mode requires a capable host implementation. The app integration
chat owns Android's privileged absolute-volume dispatcher, native USB audio-policy
mapping and reliable cleanup. API registration alone is insufficient on the
reviewed Android 17 path. No maximum-cue listening acceptance is claimed until
the host integration is activated and verified.

## Checks and artifacts

62 C suites passed under ASan/UBSan; all 63 Python tests completed successfully
with one optional test skipped. Real-SDK AVRCP volume, phone decoders and LHDC
safety checks passed. The standard float-enabled Release image passed emitted
USB descriptor and linked-memory checks. It retains 254,312 heap bytes; the
measured worst codec allocation leaves 66,272 bytes, above the unchanged
65,536-byte reserve. Float32 and S16 input are alternatives in this single image.

The private local build had already started an S16-only comparison when the
user requested its removal. CI now builds and publishes only
`airpods-dongle-pico2w-float`; both input formats remain in the regression suite.
The historical comparison is not a second supported download.

Evidence in project `airpods-pico-bringup-20261027`:
`build-receipt-0.1.45-r1.json`, `logs-0.1.45-r1/`,
`release-0.1.45-float-r1/`, and the focused lifecycle/ordering negative controls.

- Source archive SHA-256: `8d684696777554bef2a309fbe0cd49cb04458063eae9201a41d83c706922990e`.
- Standard UF2 SHA-256: `67ddd935a0d331885c2409dd234918a8844461a8db9e8790c618832369665dd0`.

Source and CMake inputs match the frozen build. Final documentation and the
single-artifact CI cleanup were added after that snapshot.

## Device validation still needed

With a paused, low-volume start, activate the verified Android host path and
confirm that software attenuation is actually delegated, physical gain follows
the phone slider, stem changes update the slider without an echo, and the maximum
tone occurs only at the shared maximum. Exercise mute, rapid mixed adjustments,
pause/resume, USB re-enumeration, AirPods/AVRCP reconnect and host-bridge death.
Verify native audio-policy cleanup, other USB routes, normal standalone use,
float playback, stems and microphone coexistence. No device operation was
performed during this investigation, and no current-consumption claim is made.
