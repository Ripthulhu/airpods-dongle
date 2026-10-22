# airpods-dongle

AirPods USB audio bridge for Raspberry Pi Pico 2 W. The board owns Bluetooth;
Windows uses its standard USB audio, media-key and serial drivers.

Based on [USBPods](https://github.com/wasdwasd0105/USBPods-Pico2W) and
[han-um's two-way audio fork](https://github.com/han-um/USBPods-Pico2W-2-way-audio-implement),
with protocol and recovery fixes from NTPods. Source revisions are pinned in
[SOURCES.json](SOURCES.json). Existing copyright and third-party licenses are retained.

## Status

Experimental. The firmware cross-compiles and the host regression tests pass.
This version has not been tested on a Pico or AirPods. The source fork reports
Pro 3 testing; that does not validate these changes.

- 48 kHz stereo playback, with AAC-ELD preferred and AAC/SBC fallback.
- Simultaneous AACP microphone, opened only by a USB recording request.
- 100 ms encoded startup silence for AAC-ELD; source audio stays buffered in order.
- Play, Pause, Next, Previous and Stop forwarded as USB media controls.
- Two saved headset slots; reconnect uses the selected address and its bond.
- Bounded recovery, packet validation and USB suspend/disconnect cleanup.

The serial port currently carries diagnostics. NTPods settings, hearing controls,
battery display and LibrePods switching are not connected to this firmware yet.
LDAC is unsupported: the inherited encoder path could exceed the negotiated packet
limit, and AirPods do not need that codec.

## Build

Requires Python 3, Git, CMake 3.17+, Ninja and a complete Arm GNU embedded C/C++
toolchain with newlib. Development builds use GCC 13.2.1 under Ubuntu 24.04/WSL.

```sh
python3 tools/prepare_sdk.py --sdk .sdk/pico-sdk
cmake -S . -B build -G Ninja -DPICO_SDK_PATH="$PWD/.sdk/pico-sdk" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Output: `build/airpods_dongle.uf2`. Hold BOOTSEL while connecting the Pico 2 W,
then copy the UF2 onto its USB drive. The original Pico W has insufficient RAM.

The SDK helper pins Pico SDK 2.1.1 and its submodules, checks original file hashes,
applies the reviewed patches, and verifies readback. Configuration and builds
reject missing patches or unknown patch states. The UF2 tool is pinned to
picotool 2.1.1. Use a separate SDK checkout rather than modifying a shared one.

## Use

Select **AirPods Dongle** as the playback and recording device. Long-press and
release BOOTSEL to pair a headset already in pairing mode. While idle, a single
press reconnects the selected headset and a double press selects the other slot
and connects it. During playback, those presses adjust volume instead.

Opening the USB microphone starts capture; closing it or suspending/unplugging
USB cancels capture. **AirPods Dongle Console** exposes bounded diagnostic logs.

## Tests and hardware checks

Run native C and Python tests on Linux/WSL:

```sh
python3 tools/test.py --sanitize
```

The tests cover malformed packets, stale connections, cancelled recording,
decoder failures, PCM ordering, media-key release/retry and SDK patch validation.
They do not measure radio behavior or acoustic latency.

Before using this firmware routinely, check USB enumeration, playback and mic
together, repeated case returns, volume/mute, stem controls, and long-running
capture. Capture the media flush flags and verify control traffic stays reliable.

The inherited 250 MHz / 1.20 V overclock needs validation on each board. Runtime
heap margin and microphone clock drift remain unmeasured. The microphone uses
the inherited 64 kHz USB presentation clock with a 48 kHz AAC coding configuration;
this does not establish 64 kHz acoustic bandwidth or calibrated recording speed.

See [implementation notes](docs/IMPLEMENTATION.md) for the reviewed differences
and the boundaries that remain untested.

## Attribution

USBPods provides USB audio, codec and Bluetooth transport code. The two-way fork
adds the AACP microphone path derived from
[LibrePods PR 655](https://github.com/librepods-org/librepods/pull/655).
NTPods supplies the reference behavior for startup priming, guarded reconnect,
packet validation and microphone recovery. See [LICENSE](LICENSE),
[LICENSE.Apache-2.0](LICENSE.Apache-2.0), and the notices in `3rd-party/` and source files.
