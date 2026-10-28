# USB volume ownership evidence

Reviewed 2026-10-28. Status: investigation, not a claim that Android USB volume synchronization works.
The 0.1.36 Pixel listener rejected the audible down/up excursion caused by
restoring the AirPods' hardware gain after forwarding a stem gesture.

## What macOS actually advertises

Saved Tahoe 25G241 / Pro 3 9A348 capture
`20261012T212353Z-4d5962ee/pairing-and-mic.pcapng`:

| Frame | Direction | Observed message |
| --- | --- | --- |
| 5018 | Mac → buds | SDP target profile descriptor: AVRCP 1.5. The request only asks for the profile descriptor, so it does not establish the target's complete supported-feature mask. |
| 5188 | Mac → buds | GetCapabilities response, events `01,02,08,09,0A,0B,0C`; no VolumeChanged (`0D`). |
| 5202 | Buds → Mac | GetCapabilities response, events `01,02,0D`. |
| 5248 | Mac → buds | RegisterNotification for the buds' VolumeChanged event. |
| 5270 | Buds → Mac | INTERIM hardware-volume report, `0x72`. |

The Mac is the controller reading the rendering buds' target volume. These
packets do not support the hypothesis that adding VolumeChanged to the Pico's
target-role event list is required to reproduce macOS behavior.

An offline inventory read all 52 saved capture files. AVRCP decoded in 27;
25 contained volume-related messages. All 29 decoded Mac event-capability
responses used the list above. No decoded bud-to-Mac SetAbsoluteVolume command
or volume-up/down passthrough was found. Captures can overlap and some start
without channel setup; these are observations, not 52 independent negative tests.
Mid-connection decoding only bound channels carrying an exact AVCTP/AVRCP
vendor header. Raw captures and the detailed inventory remain outside Git.

## A remote hardware change in the saved Mac trace

Capture `20261015T190602Z-2cc0a450/bluetooth.pcapng` contains:

| Frames | Observation |
| --- | --- |
| 3225 | Initial bud INTERIM volume `0x1F` (31). |
| 6769, 6771, 6776 | Bud CHANGED `0x27` (39); Mac re-registers; bud INTERIM remains 39. |
| 6881, 6885, 6895 | Bud CHANGED `0x2F` (47); Mac re-registers; bud INTERIM remains 47. |
| 6773 | Mac AACP `04 00 04 00 4C 00 05 00 03 01 18 1E 00`. |
| 6892 | Mac AACP `04 00 04 00 4C 00 05 00 03 01 1E 25 00`. |

There is no AVRCP SetAbsoluteVolume command anywhere in that capture, and no
captured restoration of the earlier hardware level. Saved native logs call this
`SendManualVolumeUpdateFromAccessory`, update the device volume property and
notify upward. They correlate AACP `4C`, five-byte body, subtype `03` with a manual
volume report: old/new whole percentages 24→30 and 30→37. Byte `01` and the
trailing `00` remain observed fields; their full semantics are not established.
This is feedback, **not an enable command**, and must not be replayed as an
Android volume fix. The logged scalar/dB property is not an acoustic calibration.

**Experimental-run qualification:** this run zeroed Apple SDP attributes
A000/A001/AFFF and had repeated remote disconnects after about 31 seconds.
The native Mac volume handler was running, but the run was not a stable stock
Mac listening test. Its markers only identify capture start/stop, so the exact
physical gesture is not independently marked.

## What differs on the Pixel's normal USB path

The 0.1.36 playing and follow-up snapshots keep the Pixel's UAC SET revision at
2, while completed HID volume presses increase from 30 to 45 and confirmed
AirPods gain returns to anchor 126. These are cumulative counters, not a count
of independently verified user gestures. A completed USB transfer does not
prove that Android applied exactly one volume adjustment.

The observed bud CHANGED reports and the listener's down/up excursion establish
that hardware gain changes locally and the repair later undoes it. They do not
establish an absolute phone-volume index. No marked target-role source-volume
command or passthrough currently provides a separate, gain-free gesture path.

## Consequences for the next implementation

The 0.1.37 Pixel check also failed. On its first USB start the Bluetooth stream
remained stopped; a case return restored clean stereo. On that fresh connection,
the first relayed stem change completed one HID key, restored the AirPods to
126, received ACK126, and queued a replacement volume registration. A second
attempt about 500 ms later returned `0x0C` (command disallowed). No replacement
INTERIM completed before fallback. Subsequent native CHANGED/INTERIM pairs
continued, but HID remained at one and the relay stayed disabled. The initial
successful API return proves queuing, not transmission; this is not evidence of
an explicit peer rejection. Ordinary CDC contained no matching volume-disabled
notification. The 16-entry trace retained 16 of 19 events in this window, so its
first three events are unavailable.

The listener reported a repeatable left/right imbalance after the first stem
change, cleared by pausing and resuming playback, and no working phone-volume
synchronization. Removing the former 150 ms correction wait is a possible cause
of that new balance problem, not a proven one. Do not describe faster repair as
accepted or restore that delay and call the result seamless. Firmware gain
readback is a single value and does not establish equal audible gain in both ears.

Local evidence: `volume-trace-0.1.37-case-return-extended.txt` and
`silent-state-0.1.37.txt` under the bring-up artifact directory. The listener
described the fault as repeatable; seven CHANGED reports are not seven
independently marked gestures. No further gesture test was requested after the
listener reported the imbalance.

- Keep native UAC hardware-volume hosts separate from Android's observed
  software-volume path. Do not turn their confirmed readback into a second HID
  adjustment.
- A faster repair or gain ramp may shorten/soften the excursion; it is not a
  demonstrated synchronization fix. Copying the Mac's `4C/03` feedback does not
  remove Android's software attenuation.
- Do not invert AVRCP levels into PCM gain. The protocol specifies percentages,
  not calibrated dB, and devices may quantize or clamp. A quieter local level
  would require an unknown boost; mute has no inverse. Compensation also reaches
  the buds after the queued encoded audio, not before the local change.
- A true single-authority path needs either a captured gesture that does not
  alter bud gain, or host participation coordinating its actual volume index and
  software attenuation. Keeping local hardware volume without restoring it
  avoids the bounce, but leaves the phone slider independent.

## 0.1.38 listening result and Android maximum-cue limit

The Pixel listener confirmed balanced ears, a following phone-volume slider,
and one smooth audible change. This accepts the tested relative-volume relay,
not absolute agreement between Android's slider and the AirPods' hardware gain.
The listener still heard frequent maximum-volume cues below the phone UI maximum.

Completed HID presses rose 13→74→132 during user activity, then remained at 132
with the controls untouched. These cumulative counts do not establish one event
per gesture, but no runaway key loop was confirmed in that observation.

The firmware restores hardware anchor 126; a native upward swipe can reach 127
before that restoration. This is consistent with a hardware maximum cue while
Android's separate software volume is lower. No individual cue was correlated
with a captured packet, so this remains the supported mechanism, not per-cue proof.

The Pixel's observed UAC SET revision remained 2 while HID keys and its UI advanced.
Modern AOSP AIDL USB code supports direct hardware-volume writes; normal mixed
playback can still apply software volume. Host/app-selected bit-perfect playback
is a separate optional path, not a USB descriptor switch for all apps. These
reference sources do not identify the HAL binary shipped on this Pixel.

Primary references:
[AVRCP 1.6.3, section 6.13](https://files.bluetooth.com/wp-content/uploads/dlm_uploads/2024/10/AVRCP_v1.6.3.pdf)
defines rendering volume and permits returned levels to differ from requests.
[AOSP mixer documentation](https://source.android.com/docs/core/audio/preferred-mixer-attr)
distinguishes default software volume from optional host-selected bit-perfect
hardware-volume playback. Current
[AOSP AIDL USB stream](https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/audio/aidl/default/usb/StreamUsb.cpp)
implements hardware-volume writes through the ALSA mixer; the host's playback
policy determines whether those writes track ordinary media-volume changes.
