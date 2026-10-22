# airpods-dongle

AirPods USB and wireless phone audio bridge for Raspberry Pi Pico 2 W. The board
owns Bluetooth; Windows uses its standard USB audio, media-key and serial drivers.

Based on [USBPods](https://github.com/wasdwasd0105/USBPods-Pico2W) and
[han-um's two-way audio fork](https://github.com/han-um/USBPods-Pico2W-2-way-audio-implement),
with protocol and recovery fixes from NTPods. Source revisions are pinned in
[SOURCES.json](SOURCES.json). Existing copyright and third-party licenses are retained.

## Status

Experimental firmware **0.1.8**. The firmware cross-compiles and the host
regression tests pass.
This version has not been tested on a Pico or AirPods. The source fork reports
Pro 3 testing; that does not validate these changes.

- 48 kHz stereo playback, with AAC-ELD preferred and AAC/SBC fallback.
- Phone Bluetooth input with classic aptX where negotiated and SBC fallback.
- Simultaneous AACP microphone, opened only by a USB recording request.
- Phone or microphone decoding on core 1; Bluetooth, USB and playback encoding on core 0.
- Passive BLE discovery alongside stereo playback, with bounded Apple advertisement storage.
- 100 ms encoded startup silence for AAC-ELD, sent before accepting phone playback.
- Stem media controls and volume routed to the active phone or USB input.
- NTPods USB controls and LibrePods controls over an encrypted, bonded phone connection.
- Model-gated AirPods settings, hearing profiles, saved hardware checks and app-owned ear detection.
- Selectable output codecs and supported bitrates, guarded Game Mode and performance readings.
- Nearby signal readings resolved against the selected pair's identity key.
- Two saved headset slots; reconnect uses the selected address and its bond.
- Bounded recovery, packet validation and USB suspend/disconnect cleanup.

NTPods uses the [USB control interface](docs/USB-CONTROL.md); LibrePods uses the
[phone control service](docs/CONTROL-EXTENSIONS.md). Both expose device settings
and readback without acquiring audio. USB uses Windows' built-in HID driver;
USB audio and microphone also work independently of the app. There are no
PC/phone handoff commands. Sensor streaming and case ringing are not implemented.
The serial port remains a diagnostic console.
LDAC is unsupported: the inherited encoder path could exceed the negotiated packet
limit, and AirPods do not need that codec.

## Bluetooth paths

The firmware links both Classic and BLE through one BTstack controller. The
[Pico SDK supports dual mode](https://www.raspberrypi.com/documentation/pico-sdk/networking.html#pico_btstack).

| Communication | Transport | Current implementation |
| --- | --- | --- |
| Playback and stem media controls | Classic AVDTP / AVRCP | Implemented |
| AirPods controls and HQ microphone | Classic AACP, PSM `0x1001` | Model-gated settings and readback; HQ microphone opened only by USB recording |
| Hearing profiles and saved diagnostics | ATT over Classic, PSM `0x001F` | Complete profile edits, saved checks, read-only hearing metadata and guarded in-case self-checks |
| Proximity and offline advertisements | BLE | Passive cache; selected-pair nearby finder for verified proximity reports |
| Phone app controls | Encrypted, bonded Classic RFCOMM | Same basic and settings reports as USB, with separate client transactions |
| Case DULT / Find My service | BLE GATT | Not ported yet |

BLE uses a provisional 30 ms scan window every 300 ms during stereo playback.
It pauses for Classic pairing searches, microphone startup/capture/drain and USB
suspension without a phone connection or active finder, then restores the scan policy. The CDC console reports aggregate
counts at most once per five seconds; `requested` means a scan command was queued,
not that controller acceptance was verified.

The eight-entry raw advertisement cache remains unassociated. The nearby finder
separately verifies resolvable private addresses with the selected pair's IRK
before using proximity RSSI. It shows smoothed signal, trend and last-heard age,
without metres, direction or an anonymous-device fallback. Identity remains in RAM
across the same pair's AACP disconnect, and is cleared for another pair or an
invalid key replacement. An old reading after 90 seconds does not prove absence.
The finder stops when its view/client closes, its 30-second polling lease expires
or the selected pair changes. HQ microphone use pauses scanning.
Neither path identifies a separate case transmitter or sends locator commands.
The current connection pool supports two links: phone and AirPods occupy
both during wireless playback. Passive scanning uses no connection slot.

## Build

Requires Python 3, Git, CMake 3.17+, Ninja, `qemu-system-arm` and a complete Arm GNU
embedded C/C++ toolchain with newlib. Development builds use GCC 13.2.1 under
Ubuntu 24.04/WSL.

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
Every firmware link runs the [real-codec memory check](tools/codec_probe/README.md)
against that ELF and its fingerprinted codec archive. The build fails if the
tested allocations cannot leave a 64 KiB runtime reserve.

## Use

Select **AirPods Dongle** as the USB playback and recording device. Hold BOOTSEL
for at least one second but less than three and release to pair a headset already
in pairing mode. Hold for at least three seconds to allow phone pairing for two
minutes, then select **AirPods Dongle** in the phone's Bluetooth settings. For
wireless use, power the Pico externally. USB playback and recording take priority
over phone audio. See [phone bridge](docs/PHONE-TRANSPORT.md) for details. While idle, a single
press reconnects the selected headset and a double press selects the other slot
and connects it. During playback, those presses adjust volume instead.

Opening the USB microphone starts capture; closing it, suspending USB or losing
the USB configuration cancels capture. **AirPods Dongle Console** exposes bounded diagnostic logs.

Install NTPods with its default app-only setup for dongle controls. The optional
Direct Bluetooth driver feature is only for a PC Bluetooth adapter; the dongle
needs no test-signed driver, Test Mode or host vendor-ID override. Opening NTPods'
dongle controls does not acquire audio. Select **AirPods Dongle** in Windows Sound.

## Tests and hardware checks

Run native C and Python tests on Linux/WSL:

```sh
python3 tools/test.py --sanitize
python3 tools/test_phone_codecs.py --sdk .sdk/pico-sdk --sanitize
```

The second command checks the real aptX/SBC decoders and BTstack resampler.
Use `--arm` instead of `--sanitize` for the ARM32 codec check; it requires QEMU.

The tests cover malformed packets, phone codec negotiation and controls,
early startup priming, stale connections, cancelled recording,
decoder failures, cross-core cancellation/teardown, USB control identity and
command receipts, isolated USB/phone transactions, fragmented settings transfers,
interrupted writes, model/value guards, stale profile readback, codec preflight,
Game Mode restoration, PCM ordering, media-key release/retry, BLE pause/resume,
selected-pair address rotation, finder cancellation and SDK patch validation.
They do not measure radio behavior or acoustic latency. Slot migration, failed
storage, partial console writes and button timing boundaries also have regression tests.

Before using this firmware routinely, check USB enumeration, playback and mic
together, repeated case returns, volume/mute, stem controls, and long-running
capture. Check BLE reception during playback and restoration after microphone
use. Capture the media flush flags and verify control traffic stays reliable.
Check USB reset/re-enumeration with recording open and during BLE reception.

The inherited 250 MHz / 1.20 V overclock needs validation on each board. Runtime
heap margin and microphone clock drift remain unmeasured. The microphone uses
the inherited 64 kHz USB presentation clock with a 48 kHz AAC coding configuration;
this does not establish 64 kHz acoustic bandwidth or calibrated recording speed.

Both cores have guarded 16 KiB stacks. The build measures codec allocation peaks
against the linked heap and requires at least 64 KiB left for runtime use.
The 0.1.8 link leaves 386,800 bytes of heap; the ARM codec probe peaks at
188,040 bytes, leaving 198,760 bytes before other runtime allocations.
The [codec probe](tools/codec_probe/README.md) covers the real encoders, VBR frame
bounds and decoder lifecycles; emulation does not measure full runtime heap or
stack use. Performance readings expose queue fill, dropped/underflow frames,
encode time and allocator use for hardware checks.
Phone playback releases the idle microphone decoder before preparing its input
codec and 9,772-byte worker queue; USB recording waits for worker detachment and
frees both before allocating the microphone. Diagnostic history is limited to
2 KiB, with dropped-byte counts, to leave more memory for audio.
If microphone initialization fails, reopening recording makes one fresh attempt
after any failed worker has detached.

USB audio on Android needs an OTG/power and enumeration check. Wireless stereo
bridging also needs dual-link radio and latency checks. Phone calls are not
implemented.

See [implementation notes](docs/IMPLEMENTATION.md) for the reviewed differences
and the boundaries that remain untested.

## Attribution

USBPods provides USB audio, codec and Bluetooth transport code. The two-way fork
adds the AACP microphone path derived from
[LibrePods PR 655](https://github.com/librepods-org/librepods/pull/655).
NTPods supplies the reference behavior for startup priming, guarded reconnect,
packet validation and microphone recovery. See [LICENSE](LICENSE),
[LICENSE.Apache-2.0](LICENSE.Apache-2.0), and the notices in `3rd-party/` and source files.
