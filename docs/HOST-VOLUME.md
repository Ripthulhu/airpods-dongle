# USB host volume authority

## Native-gain policy from 0.1.48

AirPods gain is the rendering volume. The USB firmware preserves native stem
steps, publishes confirmed gain through the existing UAC master Feature Unit,
and forwards explicit host UAC changes as absolute AirPods requests. It does not
generate a relative HID volume key, restore an anchor, or reserve artificial
headroom. Pause, resume and readback alone must not send a gain adjustment.
USB PCM remains unscaled in the dongle except for explicit mute.

Windows advertises hardware volume and mute for this endpoint. On the PC test
host, `IAudioEndpointVolume::QueryHardwareSupport` returned 3. This identifies
the hardware-control path; it is not a measurement of arbitrary application
gain or received PCM amplitude. The existing UAC interrupt reports a changed
master Volume CUR without generating another user gesture.

Android's ordinary USB mixer can still attenuate PCM locally. Native gain in
firmware does not change that host policy. The privileged bridge described below
must establish unity media gain and reflect the exact observed 0–127 AirPods
level into the phone UI. Acquisition must not quantize the current AirPods gain
to the phone's coarser volume index. Without a capable host bridge, native stems
still control the buds but Android's software slider remains independent.

## Origin of the host extension

Android's normal USB path can attenuate the PCM in software while the AirPods
change physical gain locally. Relaying a stem change as HID Up/Down and then
restoring the AirPods near maximum creates two volume controls. The old anchor
126 also lets each upward native gesture reach 127, causing a maximum cue below
the phone slider's maximum. A different arbitrary anchor cannot establish one
volume authority. See [captured evidence](VOLUME-EVIDENCE.md).

Candidate 0.1.45 fixes the reproduced pause/resume and AVRCP-only reconnect
lifecycle defects in the existing relay. It also provides an explicit host
authority mode for a host that delegates software volume to actual AirPods gain.
This protocol alone does not change Android mixer behavior or claim to fix the
maximum cue on an ordinary unprivileged Android USB path.

## Wire contract

Report-2 capability 128 advertises the extension. Use the existing typed report-3
feature framing, command sequence and selected-device generation fencing.

Feature `16` is a one-byte request and returns this exact 24-byte snapshot:

| Offset | Field |
| --- | --- |
| 0 | Version 1 |
| 1 | Flags: known physical gain 1, pending request 2, in-flight request 4, active authority 8, eligible USB/headset scope 16 |
| 2 | Last confirmed physical level 0–127, or 255 if unknown |
| 3 | Desired physical level while pending/in flight, or 255 |
| 4 | Physical-observation revision, u32 little-endian |
| 8 | Authority token, u32; zero unless this USB client owns a current lease |
| 12 | Remaining lease milliseconds, u32 |
| 16 | USB media generation, u32 |
| 20 | Reserved zero, u32 |

Physical observations and requested gain are separate. A successful write RPC
only queues a request; wait for pending/in-flight to clear and confirmed readback.
Revisions identify accepted device observations, not acoustic calibration or
per-ear balance. An AVRCP percentage is not a measured dB scale. Read-only phone
clients may inspect the snapshot but cannot acquire authority or receive its token.

Feature `17` is exactly seven bytes: subcommand 17, action u8, token u32
little-endian, level u8. All successful actions return the same snapshot.

| Action | Token | Level | Effect |
| --- | --- | --- | --- |
| 0 acquire | 0 | 0 | Acquire without changing gain; response contains a new nonzero token |
| 1 renew | Current token | 0 | Refresh lease |
| 2 release | Current token | 0 | Cancel unsent authority writes and end authority |
| 3 set | Current token | 0–127 | Queue absolute physical gain and refresh lease |

Only the USB client may manage authority. Acquisition requires a connected,
selected, owned headset and an available USB audio route, known gain, and no
pending volume write or native readback. Retry busy without
replaying stale desired values. All malformed values are rejected before mutation.

The lease lasts five seconds; renew at most every two seconds. It is scoped to
USB media generation, report-2 control generation, selected peer and AVRCP CID.
Pause/resume preserves the scope. USB/session/peer/channel/ownership loss, client
disconnect or expiry revokes it. The same token cannot be reused after revocation.
This independent lease does not block unrelated settings for thirty seconds.

## Gain behavior and failure handling

While leased, the firmware sends no HID volume steps, performs no anchor repair
or headroom adjustment, and does not use UAC volume initialization as another
physical-gain request. Native stem gain remains native; its fresh readback is
available to the host bridge. Accepted physical writes and device observations
advance the revision. Host notifications must update the host's state without
echoing another adjustment back to the dongle.

Release or uncertain loss cancels unsent authority writes and retains actual
gain. An already transmitted AVRCP command may finish; it must not be retried
after revocation. UAC feedback remains quiet for the rest of
that USB session because the firmware cannot know whether the host restored its
software mixer. A new valid lease may adopt fresh gain; a new USB session returns
to the standalone policy. Expiry never raises gain or restores an old anchor.

## Android host requirements

The host must establish one real gain authority before claiming synchronization.
Registering a privileged absolute-volume callback alone is insufficient in the
reviewed Android 17 implementation. Its native audio-policy absolute-volume map
must also use the selected USB route; otherwise software attenuation remains.
The native map is keyed by device type and currently ignores address, so a
prototype must guard other USB outputs of the same type. Restore prior behavior
and native policy on disconnect, cancellation and broker death. A host that cannot
provide that cleanup must leave the bridge unavailable.

Reference APIs are `AudioDeviceVolumeManager` absolute-volume dispatch and
`AudioSystem.setDeviceAbsoluteVolumeEnabled(type, address, enabled, stream)`.
Detect API signatures/capabilities at runtime. Reconcile the fresh physical and
host index while playback is paused, then enable the native path. Do not add
guessed inverse PCM gain or silence unrelated AirPods tones. Zero/mute must stay
zero through setup and cleanup. Both apps' implementation and Pixel listening
verification are separate from the firmware's regression checks.

Sources: [Android 17 manager](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android17-release/media/java/android/media/AudioDeviceVolumeManager.java),
[AudioService](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android17-release/services/core/java/com/android/server/audio/AudioService.java),
[AudioSystem](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android17-release/media/java/android/media/AudioSystem.java),
[native policy](https://android.googlesource.com/platform/frameworks/av/+/refs/heads/android17-release/services/audiopolicy/managerdefault/AudioPolicyManager.cpp).
