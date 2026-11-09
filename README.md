# AirPods dongle

AirPods USB audio and Bluetooth phone relay for **Raspberry Pi Pico 2 W**.
With the CYW43439
`.0076` radio path and float32, S32 and S16 USB input.

## Download firmware

**[Download firmware (UF2)](https://github.com/Ripthulhu/airpods-dongle/releases/latest/download/airpods_dongle.uf2)**

[Current release: notes, checksums, source and tools](https://github.com/Ripthulhu/airpods-dongle/releases/latest)

This repository is private. Sign into GitHub with an account that has access to
open the release or download its files. Use the same `airpods_dongle.uf2` for
either Pico 2 W; the host selects the USB sample format at runtime.

The original Pico W has insufficient RAM. USB use requires a data cable, a
USB Audio Class 2 host and sufficient power; phones also need USB host/OTG support.

## Install or update

1. Pause playback and microphone use.
2. Hold the Pico's **BOOTSEL** button while connecting it to a PC by USB.
3. Release the button when its USB boot drive appears, then copy
   `airpods_dongle.uf2` onto that drive. The Pico restarts automatically.
4. Connect or pair your AirPods using the controls below.

Each board retains its own identity and pairing/settings storage during a
firmware update. Keep full-flash backups associated with their original board.

## Use

For USB audio, select **AirPods Dongle** in the host's sound settings. Windows
uses its built-in USB drivers. NTPods' default app-only installation provides
optional dongle controls; playback and stem controls also work without the app.
LibrePods can control the dongle from Android.

With firmware 0.1.65, Conversation Awareness music lowering and USB Automatic
Ear Detection also run in the dongle. Windows/Linux need no companion app or
extra driver for those actions. Enable their existing AirPods settings through
the [CLI](docs/CLI.md); see [behavior and host/player limits](docs/releases/0.1.65.md).

For Bluetooth relay, pause audio and hold BOOTSEL for at least three seconds,
then release it. Pair **AirPods Dongle** in the phone's Bluetooth settings within
two minutes. Keep the Pico powered by USB or an external supply.

Connecting the phone's Bluetooth music profile selects relay mode and removes
the USB audio interfaces. Disconnect that Bluetooth connection to return to USB;
a music pause keeps relay mode selected. With the AirPods disconnected, USB
exposes controls only. See the [phone bridge guide](docs/PHONE-TRANSPORT.md).

### Buttons and connection policy

| BOOTSEL action | Result |
| --- | --- |
| One short press | Request the selected saved AirPods and audio takeover. With no saved pair, start discovery. |
| Double press | Toggle Game Mode for supported AirPods/codec combinations. Game Mode starts off after power-up. |
| Hold for at least 1 second and release before 3 seconds | Start AirPods pairing. Put both buds in the open case with its light flashing white first. |
| Hold for at least 3 seconds, then release | Open the phone's Bluetooth pairing window for two minutes; pause audio first. |

The dongle waits at startup and after a disconnect. A button/app Connect request
starts a bounded reconnect attempt; the AirPods may also initiate their own
connection, including when their case opens. Opening app settings or beginning
playback does not claim the AirPods from another host.

The LED is solid while the AirPods are connected and off while disconnected.
AirPods discovery blinks.
Enabling Game Mode gives two brief pulses; disabling it gives three seconds dark
before the LED returns to its connection indication.

### App controls and command line

- [USB controls used by NTPods](docs/USB-CONTROL.md)
- [Bluetooth controls used by LibrePods](docs/CONTROL-EXTENSIONS.md)
- [Hardware volume and Android host integration](docs/HOST-VOLUME.md)
- [AirPods CLI instructions](docs/CLI.md) · [Download CLI ZIP](https://github.com/Ripthulhu/airpods-dongle/releases/latest/download/airpods-cli.zip)

The Python CLI uses the existing USB HID protocol on Windows, Linux and Android:

```sh
python tools/airpods.py help
python tools/airpods.py status
python tools/airpods.py connect
python tools/airpods.py disconnect
python tools/airpods.py pair phone
python tools/airpods.py reconnect-policy manual
python tools/airpods.py get mode
python tools/airpods.py set mode transparency
```

Use `pair` for new AirPods, or `connect` for an existing bond. Replacing saved
AirPods requires `pair airpods --replace`. Reconnect is manual by default;
`reconnect-policy auto` explicitly allows reclaiming AirPods from another host.
See [CLI help and connection behavior](docs/CLI.md) for confirmation and firmware requirements.

The serial port is a diagnostic console. See [runtime USB bootloader entry](docs/USB-CONTROL.md#usb-bootloader-entry)
for updating through the console instead of the physical button.

## Status and limits

Current source version: **0.1.66**. See the [release notes](docs/releases/0.1.66.md).
The [Game Mode transition](docs/GAME-TRANSITION.md)
and [Android/dongle protocol alignment](docs/ANDROID-PROTOCOL-PARITY.md) describe
its implementation and validation boundaries.

- Playback is 48 kHz stereo, with AAC-ELD preferred and AAC/SBC fallback.
- Bluetooth relay supports guarded LHDC, aptX and SBC phone input.
- Bluetooth phone calls remain experimental and disabled by default. See the
  [phone transport record](docs/PHONE-TRANSPORT.md).
- Sensor streaming and case ringing are not implemented.

A successful CI build verifies code, codec and linked-memory checks. Hardware
listening, weak-signal behavior, microphone lifecycle and end-to-end latency
need their own evidence. The [development history](docs/HISTORY.md) preserves
previous measurements and acceptance records.

## Build

[Builds and check reports](https://github.com/Ripthulhu/airpods-dongle/actions/workflows/firmware.yml)
are available in GitHub Actions. Successful `main` builds publish the latest
unified firmware release; its tag and `COMMIT.txt` identify the exact source.
`BUILD.json` records the build options and artifact hashes.
When changing the firmware version, add `docs/releases/<version>.md` with the
user-visible changes, usage and verification limits. Publication refuses missing
or blank version notes before making changes on GitHub.

Local builds need Python 3, Git, CMake 3.17+, Ninja, a complete Arm GNU embedded
C/C++ toolchain with newlib, and `qemu-system-arm`. Use a separate SDK checkout:

```sh
python3 tools/prepare_sdk.py --sdk .sdk/pico-sdk
cmake -S . -B build -G Ninja -DPICO_SDK_PATH="$PWD/.sdk/pico-sdk" -DCMAKE_BUILD_TYPE=Release -DAIRPODS_EXPERIMENTAL_RADIO0076=ON
cmake --build build --parallel
```

The output is `build/airpods_dongle.uf2`. The SDK helper pins Pico SDK 2.3.1,
checks source hashes and applies the reviewed transport patches. Builds reject
unknown patch states and run the [linked codec/memory gate](tools/codec_probe/README.md).

## Tests and hardware checks

```sh
python3 tools/test.py --sanitize
python3 tools/test_volume_sdk.py --sdk .sdk/pico-sdk --sanitize
python3 tools/test_phone_codecs.py --sdk .sdk/pico-sdk --sanitize
python3 tools/lhdc_probe/run.py --sanitize
```

See [implementation notes](docs/IMPLEMENTATION.md),
[radio transport](docs/CYW43-TRANSPORT.md) and the
[archived test and hardware records](docs/HISTORY.md#tests-and-hardware-checks)
for the broader checks and remaining limits.

## Attribution

Based on [USBPods](https://github.com/wasdwasd0105/USBPods-Pico2W) and
[han-um's two-way audio fork](https://github.com/han-um/USBPods-Pico2W-2-way-audio-implement).
The AACP microphone path derives from
[LibrePods PR 655](https://github.com/librepods-org/librepods/pull/655).
NTPods supplies reference behavior for startup, ownership and recovery.

Source revisions are pinned in [SOURCES.json](SOURCES.json). Existing copyright
and third-party licenses are retained in [LICENSE](LICENSE),
[LICENSE.Apache-2.0](LICENSE.Apache-2.0), the source files and `3rd-party/` notices.
The release also includes `THIRD-PARTY-NOTICES.txt` and the pinned source metadata.
