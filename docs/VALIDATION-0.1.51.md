# Automatic AAC-ELD bitrate and connected-link diagnostics — 0.1.51

The 0.1.50 outdoor field test failed on the Pixel's USB route, including two
complete disconnections. The Pico remains integrated into the phone case;
moving its antenna is not an acceptance prerequisite. This release adds live
bitrate adaptation and the missing radio evidence. It does not declare the
outdoor reliability problem solved before a repeat field test.

## Behavior

AAC-ELD automatically lowers its encoder budget after persistent controller
flush or transport-pressure events. Reductions are bounded by a 128 kbps floor
and the selected/negotiated ceiling. Recovery is deliberately slower than
reduction, requiring sustained clean observations before increasing one step.
The user's CBR/VBR selection and bitrate ceiling remain unchanged. The adaptive
budget is reported separately from the negotiated ceiling.

The normal pressure window is one second. Two consecutive bad windows, or at
least three flush events within a window, lower one step. A clean window has no
flush and fewer than two local transport-pressure events. Raising one step
requires 30 continuous clean seconds. Initial eligibility has a three-second
settling period. The ladder is 128, 174, 192, 220 and 256 kbps, limited by the
negotiated ceiling; VBR budgets round down to whole bytes per 10 ms frame.
These are implementation heuristics, not a claimed Apple RF-adaptation protocol.

The live update occurs between encoding units with no queued partial packet.
It retains the encoder, stream configuration, frame counter, RTP clock and
unconsumed PCM. Already encoded audio is not discarded to apply a new budget.
The new encoder configuration is checked; a rejected update is rolled back
before more PCM is consumed. Startup, source loss, Game Mode transitions and
microphone activity suspend adaptation so those events are not misclassified
as a bad radio link.

A specific send timeout queues a lower budget before recovery stops the stream.
That decrease can survive recovery on the same connection and apply once the
source is warm and the encoder is at a safe boundary, without waiting another
three seconds. Otherwise repeated stalls could keep restarting the settling
period forever. PCM starvation alone cannot take this shortcut. Changing the
source, ownership, connection, Game Mode request or microphone state clears it.

The real encoder test also exposed an existing CBR alignment case: a 220 kbps
unit can occupy 276 bytes. Packet reservations now allow the extra byte before
consuming PCM, including when the bitrate changes live.

Radio observations are read-only and apply to the selected AirPods connection.
Measurements include Classic RSSI, link quality, current/maximum transmit power
and the actual AFH mode/channel map where supported. They have independent
validity and age fields. An idle audio transport does not poll the controller;
a host that keeps an active stream open with silence still counts as active. Neither
reading a diagnostic snapshot nor starting the host collector enables packet
logging or changes the radio configuration.

Feature 19 and `tools/audio_diagnostics.py --radio` expose the cached observations
and adaptation decisions on Windows or the rooted Pixel. The collector samples
every two seconds. Exact fields and unknown-value rules are in
[CONTROL-EXTENSIONS.md](CONTROL-EXTENSIONS.md#radio-and-automatic-bitrate-snapshot).

## Validation status

Implementation and independent review are complete. The final r2 run passed all
eight build stages, each with exit status zero: pinned SDK preparation, Release
configuration, float-capable firmware build, phone-codec checks, SDK volume
checks, regressions, real-encoder adaptation checks and LHDC safety checks.
The regression log records 63 C sanitizer checks and 65 Python tests, with one
optional skip. The real FDK encoder/decoder checks passed 14 CBR/VBR cases and
4,416 stereo access units; deliberately recreating the encoder failed the
continuity check as intended. Tests exercise actual transport-stop handling,
same-link urgent reductions, microphone/Game Mode/source cancellation,
controller-reply epochs, timer wrap and cached diagnostic bounds.

The linked-memory check passed with 65,944 bytes remaining at the emulated
worst-case codec allocation, against the required 65,536-byte runtime reserve.
Descriptor validation confirms both stereo 48 kHz USB alternatives: 16-bit PCM
on alternate setting 1 and IEEE 32-bit float on alternate setting 2. The float
endpoint allows 392-byte packets. This verifies the built descriptor; actual
selected-format and listening checks on this firmware remain pending.

Frozen r2 source archive SHA-256:
`7907c65dd56ce21c12e2260206cd4b04c81723ed86d287913310631a53fb5f32`.
Verified UF2 SHA-256:
`a429561c3d59cedf020b686846474acb79601a54ad5bb3c606e0bf4b20f53a41`.
The local validation records are
`outputs/airpods-pico-bringup-20261027/logs-0.1.51-r2.tgz` and
`outputs/airpods-pico-bringup-20261027/release-0.1.51-float-r2/memory-check.json`.

The user first reported clean PC Game Mode audio from another room on 0.1.50,
then reported hitches while close to the PC. It is not a proven-clean reference.
The user subsequently requested updating both boards. Both received this exact
0.1.51 image, with full private flash backups, readback verification and unchanged
64 KiB pairing tails. The PC serial is `8B39F1F49316A397`; the phone serial is
`DC1F379C3B1EFCA5`. The phone itself and its apps were not restarted or modified.

The PC selected genuine float USB alternate setting 2. All five radio fields
returned valid data with no query failures. On the idle phone dongle, radio
sampling was stopped as intended. During PC Game Mode playback, the 90-snapshot
radio capture covered 178 seconds and showed live 256 -> 220 -> 256 kbps changes
without encoder errors or an ACL epoch change. The listener nevertheless reported
hitches at 256, the same or worse at 220, and finally stuttering more often than
every ten seconds. Game Mode was turned off at the end of this failed comparison.

Source snapshots spanning a longer interval than the radio capture showed no
active PCM drops, zero insertion, malformed USB packets or timeline recoveries,
while the controller flush count increased by 24. Maximum encoding-unit time
since boot was 7,011 us for 10 ms of audio; maximum send wait was 8 ms and maximum
send gap 40.243 ms. These readings do not establish CPU saturation or an exact
acoustic correlation with each flush. RSSI stayed near the Classic reference
window and link quality remained 255 despite the audible failure; neither can
be treated as proof of good audio delivery.

The run also exposed a policy weakness: separated single-loss windows were
forgotten after one clean second, so recurring sparse losses did not reliably
produce further reductions. The follow-up 0.1.52 corrects that sensitivity.
Hardware acceptance and outdoor improvement remain unverified.

Required device checks: live budget reductions/recovery with uninterrupted
stereo, unchanged native volume behavior, both USB sample formats, ordinary and
Game Mode playback, microphone transitions, case-return reconnect and idle
sampling stop. Repeat the outdoor USB test with the Pico in its permanent case
and record sparse radio snapshots. A clean host test does not establish outdoor
radio reliability or guarantee that the link cannot disconnect.

Classic RSSI is relative to the controller's receive-power window; link quality
is vendor-defined. Neither supplies a universal absolute-strength threshold.
Completed packet credits are not proof of delivery, and a flush event need not
be audible. See the [Bluetooth HCI command definitions](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-61/out/en/host-controller-interface/host-controller-interface-functional-specification.html).
