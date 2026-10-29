# Native AirPods volume — 0.1.48

## Problem and behavior

The 0.1.47 listener still heard volume dip farther than its final level after a
stem swipe. That path let the AirPods apply their native change, sent a relative
USB HID volume key, then restored a hardware anchor or applied a different host
step. Preserving fine host requests fixed a sticking point but did not eliminate
these competing adjustments.

Version 0.1.48 removes that relative-volume relay and the anchor/headroom repair.
A confirmed native AirPods change stays at its new 0–127 level. The dongle sends
a USB AudioControl master-volume notification; it does not turn the observation
into another volume request. Explicit host slider changes still send absolute
AirPods commands. USB mute remains separate and retains gain for unmute.

The USB input path has no volume multiplier. Production-source UAC regressions
verify byte-identical stereo float and S16 samples at minimum, middle and maximum
Feature Unit volume. Explicit mute alone zeros them. Tests also check the exact
notification bytes and the coalesced latest GET_CUR response.

## Hosts

Windows reports hardware volume and mute for the current endpoint (support mask
3). This permits its endpoint slider to control the AirPods through the existing
UAC Feature Unit, with no companion required. It does not override volume chosen
inside an individual player or prove measured USB sample amplitude on its own.

Android's ordinary USB software mixer is a separate host issue. The app-owned
privileged absolute-volume bridge must establish unity media gain, preserve the
fresh physical level during acquisition, and mirror it to the phone display.
Firmware alone cannot change Android audio policy. The bridge and physical
listening checks remain required before claiming Android gain synchronization.

The prior Game Mode controller-timeout correction is retained unchanged. The
current volume changes do not establish that the remaining hitches are fixed.

## Ordering checks

Production-handler fixtures reproduce a native change arriving while an older
host write awaits its ACK and another request is queued. The native change must
not be undone by that older queued request. Matching SET echoes and actual
clamped ACKs remain distinguishable from a later accessory change; a genuinely
newer host request can still win. The overlap has a bounded timeout in both
standalone and leased operation. Baseline-only INTERIM, repeated changes,
duplicate CHANGED, missing readback and counter wrap are covered.

A second fixture loses audio ownership and replaces the AVRCP channel before
the next poll. That order must not turn an old owner's pending request into a
new standby request. These are reproduced asynchronous-order cases, not claims
that the hardware captures contained every sequence.

## Validation state

All 62 C sanitizer checks and 63 Python checks passed (one optional skip).
SDK AVRCP, phone-codec and LHDC checks passed, as did the standard float-capable
firmware build, descriptor validation and linked-memory check. The worst codec
allocation leaves 66,208 bytes against the 65,536-byte runtime reserve. Separate
old-code checks fail all four native-gain requirements: step retention, maximum
preservation, pause and AVRCP reconnect. Independent source review and
production-handler replays found no remaining blocking issue.

The frozen source archive is
`b143419ab16d4c8db311f285f66eb9cab15ec22364c6673353ca69d61574ecb6`.
The checked float-capable UF2 is
`04d3477fee7e95ddec19876ec925ad708c98efcaea7b96802066458f2a65061b`.

The original PC board was backed up, flashed and verified as 0.1.48. Pairing
storage matched byte for byte. It reconnected and confirmed the normal 200-ms
controller timeout. Windows still reports hardware volume and mute.

During the native-volume comparison, the listener reported that it was much
better in response to the no-rebound/balanced-ears prompt. The bounded trace
recorded native increments of 8 up to actual 127, no HID volume transfers, no
anchor/restoration state, and one initial absolute-write ACK. Its 16-record ring
retained the tail of 36 events, so it does not preserve every initial gesture.
The normal playback snapshot confirmed float USB alternative 2.

Pause/reconnect volume retention, host PCM amplitude measurement and the Android
bridge still need device verification. The phone board remains on 0.1.46. Game
Mode hitch acceptance is separate. Its live transition has now confirmed the
60-ms controller timeout on the same AirPods ACL, with no pending or failed
command. A four-minute listening comparison is in progress. Detailed volume
capture is off by default; the bounded volume comparison capture is now closed.
