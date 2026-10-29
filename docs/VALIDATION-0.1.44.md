# USB headset presence — 0.1.44

USB audio is now available only while the selected headset has a real outgoing
Bluetooth audio-profile signaling connection. The Pico boots as HID/CDC only
(`CAFE:4005`); it does not briefly advertise headphones during initialization.
Headset connection restores the audio composite (`CAFE:4015`) unless the existing
Bluetooth phone route excludes USB audio. Both control protocols and the serial
identity are retained through the existing 250 ms re-enumeration.

Headset absence overrides an open USB microphone. It rejects late audio data and
interface requests immediately after publication; USB teardown then clears both
audio alternates and publishes microphone close. An accepted USB microphone still
has priority over Bluetooth input while the headset is present.

Presence deliberately follows the accepted selected-headset profile connection,
not active playback, AACP readiness or audio ownership. Pausing music, Game Mode
adjustments, microphone recovery and two-host standby do not remove USB audio.
An old connection's delayed release cannot hide the current headset. Rapid
disconnect/reconnect changes coalesce to the latest state during the detach.

This lets the idle service reductions from 0.1.43 engage when the buds disconnect,
even if the host would otherwise hold a silent USB stream open. It is not a claim
of measured power savings; the Bluetooth radio and USB controls remain powered.

## Focused evidence

- Production UAC callbacks cover cold boot, normal return, late callbacks,
  Bluetooth exclusion, absence during duplex recording, closed interface state,
  rapid connection changes and timer wrap. Both USB format variants pass under
  ASan/UBSan.
- The production descriptor selector starts with matching control-only device
  and configuration descriptors and switches both together.
- The production connection getter is checked through idle Suspend/Start,
  ownership loss/return, actual signaling loss and stale release after reconnect.
  Negative controls fail when presence is incorrectly tied to streaming or the
  stale-release guard is removed.
- Independent review found no remaining blocker. The control-only host mount is
  checked separately from audio-route availability; HID/CDC stay usable.

## Full build validation

61 C regression suites passed with ASan/UBSan. All 63 Python tests completed
successfully (one optional test skipped). Real-SDK volume and phone-codec tests,
plus LHDC safety checks, passed. Both float-enabled and S16-only Release firmware
built with the pinned SDK; emitted USB descriptors and linked-memory checks passed.

The float image retains 66,312 bytes after the measured worst codec allocation;
S16-only retains 67,096 bytes. Both exceed the unchanged 65,536-byte reserve.
The float image contains the actual IEEE float32 alternative, with stereo
four-byte samples and a 392-byte maximum packet; the microphone remains S16.

Reproducible build evidence is stored in project `airpods-pico-bringup-20261027`,
under `build-receipt-0.1.44-r1.json`, `logs-0.1.44-r1/` and
`release-0.1.44-{float,s16}-r1/`. SHA-256 values:

| Artifact | SHA-256 |
| --- | --- |
| Source archive | `7c3902183c91d77c4788dcebbeb49541a43937ac84009b8a9523c9845549d491` |
| Float-enabled UF2 | `b4bd63e85d1fe8c2cad8e055dbb374d47b7a4ee472e1e171ef21ba07e9db8395` |
| S16-only UF2 | `f351ae8eae4ba4048e9e7e841e09a5e800c1fc0554a9056c766526082a1b0585` |

Final validation documentation was added after the frozen build snapshot; code,
configuration and test hashes match that snapshot.

## Deployment and startup checks — 29 October 2026

Both lab Picos were updated from 0.1.42 to the float-enabled 0.1.44 image above.
Each board's chip identity, old/new firmware version and 4 MiB flash size were
checked. A complete private backup was taken before programming, `picotool load
-v` verified the image, and the last 64 KiB containing pairing data matched byte
for byte before reboot. Both boards rebooted and answered HID and CDC diagnostics.

The disconnected PC dongle enumerated as control-only `CAFE:4005`, with no USB
audio endpoint. The Pixel dongle automatically reconnected to the AirPods and
restored `CAFE:4015` and the Android USB audio card. Its playback and capture were
stopped. Game Mode was off in the startup observations. Android's automatic USB
routing preference remained `null` before and after deployment. Full backups and
per-board verification receipts are retained privately in the lab project.

The old phone firmware refused remote BOOTSEL while reported playback, capture
and call activity were idle. Physical BOOTSEL was used for that board. A stale
playback transition is a review candidate; the exact busy predicate was not
observed, and this deployment does not claim to fix it.

## Hardware acceptance still needed

Listening and current-consumption tests have not been performed on this image.
On both hosts, repeat cold USB attachment with the buds asleep and wake/connect
checks, then verify both USB audio formats and microphone availability. Pause,
Game Mode switching and standby takeover must keep the device present. Put the
buds away and verify that both audio endpoints disappear; repeat with an open
microphone. Check case return and Bluetooth phone mode without manually changing
the USB route. The operating system decides which output it selects after a
device removal or arrival; firmware does not force the host's default volume or
output. Keep the previous firmware and saved pairing backup for rollback.
