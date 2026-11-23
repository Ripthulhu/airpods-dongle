# Documentation

Living reference documents, kept current with the firmware:

- [USB-CONTROL.md](USB-CONTROL.md): the version-1 USB HID control reports, media keys and bootloader entry.
- [CONTROL-EXTENSIONS.md](CONTROL-EXTENSIONS.md): the typed control features layered on version 1 (settings, output profiles, Game Mode, performance, nearby finding).
- [CLI.md](CLI.md): how to run and use the Python command-line client.
- [SETTINGS-PARITY.md](SETTINGS-PARITY.md): which AirPods settings the firmware and CLI support compared with LibrePods.
- [HOST-VOLUME.md](HOST-VOLUME.md): how USB host volume and AirPods gain share authority.
- [VOLUME-EVIDENCE.md](VOLUME-EVIDENCE.md): the dated measurements and regression evidence behind the volume policy.
- [AUDIO-CONTINUITY.md](AUDIO-CONTINUITY.md): USB audio continuity, PCM formats and float transport.
- [USB Game Mode transitions](GAME-TRANSITION.md) covers playback timing and receiver-buffer recovery.
- [PHONE-TRANSPORT.md](PHONE-TRANSPORT.md): the phone Bluetooth bridge, audio ownership and the experimental call path.
- [CYW43-TRANSPORT.md](CYW43-TRANSPORT.md): the shared CYW43 bus repair carried as SDK patches.
- [IMPLEMENTATION.md](IMPLEMENTATION.md): reviewed differences from the upstream forks this firmware derives from.
- [ANDROID-PROTOCOL-PARITY.md](ANDROID-PROTOCOL-PARITY.md): the dongle and Android client protocol comparison.
- [AUDIT.md](AUDIT.md): the phone bridge and NTPods USB audit.
- [HISTORY.md](HISTORY.md): the earlier README with per-version status and hardware checks.
- [releases/](releases/): release notes, one file per firmware version.

The machine-readable control protocol lives in [../protocol/](../protocol/README.md).

[archive/](archive/) holds dated, one-off records: per-version validation runs,
earlier audits and dated investigation checkpoints. They are kept unchanged as
evidence and are not updated when the firmware changes.
