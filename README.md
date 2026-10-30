# airpods-dongle

[![Firmware CI](https://github.com/Ripthulhu/airpods-dongle/actions/workflows/firmware.yml/badge.svg)](https://github.com/Ripthulhu/airpods-dongle/actions/workflows/firmware.yml)

AirPods USB and wireless phone audio bridge for Raspberry Pi Pico 2 W. The board
owns Bluetooth; Windows uses its standard USB audio, media-key and serial drivers.

Based on [USBPods](https://github.com/wasdwasd0105/USBPods-Pico2W) and
[han-um's two-way audio fork](https://github.com/han-um/USBPods-Pico2W-2-way-audio-implement),
with protocol and recovery fixes from NTPods. Source revisions are pinned in
[SOURCES.json](SOURCES.json). Existing copyright and third-party licenses are retained.

## Status

Saved good builds, including the startup LED correction, are available for the
[PC (0.1.54-r7) and phone (0.1.57-r6)](docs/LED-STARTUP-FIX-20261030.md), with
matching source, firmware, checksums and validation receipts. Both are installed
with pairing/settings preserved. The
[controller-firmware investigation is paused](docs/CYW43439-RESEARCH-CHECKPOINT-20261030.md);
its unsuccessful experimental firmware is not installed.

Candidate **0.1.57** repairs reproduced silent packet loss when the CYW43
host-to-controller transfer ring fills. It retains the blocked packet and retries
through BTstack's asynchronous transport contract. This is an offline candidate;
relay listening acceptance remains open. See the
[transport investigation](docs/VALIDATION-0.1.57.md).

Candidate **0.1.48** preserves native AirPods volume steps and publishes the
confirmed level through USB hardware volume. It removes the relative HID key
and gain-restoration round trip that caused an audible dip and rebound. Android
needs its privileged host bridge to remove the separate software attenuation;
firmware alone cannot set Android's mixer policy. Full regression, codec,
firmware and linked-memory checks pass. The PC board is updated with pairing
preserved; the listener accepted the native-volume comparison as much better.
Android and extended lifecycle checks remain pending.
See [validation](docs/VALIDATION-0.1.48.md) and
[native gain and host integration](docs/HOST-VOLUME.md).

Candidate **0.1.47** preserves fine USB volume requests when the AirPods confirm
their coarser volume step, and rejects older pending volume readback after a
newer accepted write. Game Mode now requests the captured 60-ms controller flush
timeout on the selected AirPods connection, restoring 200 ms outside that mode.
Build and regression checks pass. The PC board is updated with pairing preserved;
normal controller timeout is confirmed. The listener still reported the
stem-volume dip/rebound; 0.1.48 replaces that underlying relay. Game Mode
listening validation remains pending.
See [validation](docs/VALIDATION-0.1.47.md).

Candidate **0.1.46** allows an explicit runtime BOOTSEL request during playback,
recording, calls or audio transitions. It interrupts audio through the SDK's
chip reset after a bounded reply delay, retaining DTR/session fencing and board
identity checks. It does not wait for Android to release USB audio. Older firmware
may need one physical BOOTSEL update before this works. Both boards were tested
on 0.1.46, with pairing storage verified unchanged. The Pixel's USB playback was
active when remote BOOTSEL entry passed; the board restarted and the listener
confirmed music returned in both ears. See [validation](docs/VALIDATION-0.1.46.md)
and the [runtime update protocol](docs/USB-CONTROL.md#usb-bootloader-entry).

Candidate **0.1.45** fixes volume drift across pause/resume and cancels stale
internal gain repairs after an AVRCP reconnect. It adds a bounded, explicit USB
host volume authority contract so a capable host can use actual AirPods gain
without duplicate HID steps or near-maximum anchor restoration. Android's normal
software-volume path requires privileged host integration to eliminate premature
maximum cues; the new firmware API alone does not establish that integration.
See [host volume](docs/HOST-VOLUME.md). These changes are included in 0.1.46;
Android host integration and maximum-cue listening acceptance remain open.

Candidate **0.1.44** presents USB headphones and microphone only while the
selected AirPods have a Bluetooth audio-profile connection. Boot and a real
AirPods disconnect expose only HID/CDC controls; reconnect restores audio unless
the phone's Bluetooth route owns it. Paused music, Game Mode changes and standby
takeovers retain the connection and do not remove the USB device. AirPods absence
overrides an open USB microphone. This also lets the idle-power changes below
engage when the host would otherwise keep an unused USB audio stream open.
Both builds and all offline checks passed. Both lab dongles previously ran
0.1.44, pairing storage verified unchanged. Startup checks saw control-only USB
on the disconnected PC dongle and automatic audio return after the Pixel dongle
reconnected to the AirPods. Listening and power measurements remain open.
See [0.1.44 validation and hardware checks](docs/VALIDATION-0.1.44.md).

Candidate **0.1.43** adds the renewed audit fixes: bounded, cancelable takeover
Pause; explicit automatic USB Pause without changing stem toggles; and rejection
of an SBC payload too small for a frame. A sample-equivalent unity-rate resampler
optimisation reduces that render path's emulated instruction work by 26–29%.
Idle USB service drops from 2,000 to 250 calls per second when audio endpoints
and the diagnostic terminal are closed; the fully idle main-loop target drops
from 1,000 to 200 passes per second. Audio and transition paths retain their fast
cadence. See the [0.1.42 re-audit and 0.1.43 changes](docs/AUDIT-0.1.42.md)
for evidence, host limits and power-measurement requirements. These are offline
results; 0.1.43 is not flashed or listening/power accepted.

Candidate **0.1.42** corrects an overflow-accounting feedback loop reproduced
with perfectly timed phone input: cancelling healthy output deadlines kept the
full buffer from draining. It also detects accumulated output timing loss across
short stalls and uses the existing receiver-confirmed restart, instead of letting
playback remain silent after the sender catches up. Hearing the final candidate
on hardware remains necessary; these are regression results, not audio acceptance.
Both lab dongles previously ran 0.1.42, with pairing storage preserved. All
build checks passed, and a silent Windows stream verified actual IEEE float32
USB input. See [0.1.42 validation and next checks](docs/VALIDATION-0.1.42.md).

Version **0.1.41** keeps USB audio absent for the selected Bluetooth phone
connection, including paused playback, failed media setup and retries. An
authenticated Bluetooth music connection selects this mode; disconnecting the
phone's entire Bluetooth connection restores USB audio. Idle call/control links
alone do not select it. CDC/HID return after a brief USB re-enumeration. An
existing USB microphone request retains priority. Its Pixel route checks passed,
including loss of the media profile with the phone connection still present.
Its Bluetooth playback still failed; 0.1.42 corrects the scheduling defects above.
See the [renewed firmware audit](docs/AUDIT-0.1.39.md) for reproduced faults,
rejected buffer-only fixes, and hardware checks still needed.

Version **0.1.40** restored USB too early after a failed media startup; 0.1.41
corrected that mode lifetime. Both versions' overflow accounting could cancel
future output deadlines; 0.1.42 only credits losses against work already overdue.

Development candidate **0.1.39** verifies USB Game Mode timing after startup
priming and starts in normal mode; double-press BOOTSEL to enable Game Mode.
Version **0.1.38** can confirm the receiver-buffer preparation before
Start, then miss the live timing adjustment after its 100 ms priming. A PC hitch
investigation found continuous host/USB/encoder data and clean playback after a
manual off/on retune. The original PC Pico passed short normal-mode,
double-press Game Mode and automatic case-return checks on 0.1.39. Its startup
trace confirms the fresh post-prime request and completed adjustment. The second
Pico also passed short Pixel USB S16 stereo playback on 0.1.39 in
normal mode after manual takeover. Extended playback, microphone and phone-call
checks remain open. The 60 ms target and
packet grouping are unchanged.

Version **0.1.38** removes the unsuccessful extra AVRCP volume
refresh, restores a short settling delay, and adds AACP audio ownership and a
paired BOOTSEL single-press takeover request. It also allows a fresh USB playback
start to restart a peer-suspended stream while this dongle owns audio. These
changes passed short Pixel and Windows stereo/relative-volume checks. Single-press
takeover passed in both directions without reconnect sounds, and genuine float32
USB input was verified. Android's premature maximum-volume cues remain unresolved;
the software slider and physical gain do not yet share one absolute authority.
Extended playback and call acceptance remain open. **0.1.37** failed: initial playback was
silent until case-return reconnect, then a stem swipe disturbed left/right
balance and failed to synchronize the phone. **0.1.36** also failed its volume
check: phone and stem volume could diverge, with an audible second gain step.
Version **0.1.36** keeps USB resampling continuous across short input and renews
the phone-call retry budget when its authenticated phone returns. Initial Pixel
USB S16 playback with Game Mode on was clean in both ears; this is not a claim
that every reported Game Mode crackle is resolved. Volume validation remains open.
Version **0.1.35** adds manual volume-path diagnostics.
The **0.1.34** USB stem-volume relay passed offline checks but did not synchronize
the Pixel's volume in its live test. The earlier Windows UAC synchronization
worked; preserve that path while investigating Android's software-volume behavior.
Version **0.1.33** adds an HFP phone-call bridge with CVSD
voice audio, retaining **0.1.32**'s 90 ms phone jitter reserve and new
arrival/delivery timing counters. Bluetooth calls and the phone jitter reserve
still need hardware validation. A five-minute Pixel USB S16 run on 0.1.33 with
Game Mode on was clean, with no new active sample loss or recovery. Two-unit
AAC-ELD packets in **0.1.31** reduced send waits substantially, but occasional
hitches remained. Version **0.1.25** passed short
Windows S16 and genuine IEEE
float32 USB listening checks, including microphone open/close with normal stereo
afterward. Windows' device format and the Pico's selected alt 2 confirm float
transport; volume and mute were unchanged. The listener accepted the remaining
microphone transition behavior. One opening resync and the deliberate closing
repair remain in the counters. Full 30-minute/path acceptance and the updated
Android/Symfonium checks remain open.

Standalone Windows and Pixel USB playback, stem pause/resume and saved-pair/
case-return reconnect have been exercised with Pro 3 firmware 9A348. Game Mode
starts off at power-up. A prior visual/listening test with it enabled measured
approximately 60 ms total speaker latency. With a saved AirPods pair, a single BOOTSEL press requests audio
takeover; without one, it starts discovery. Double-pressing BOOTSEL toggles Game Mode: two LED pulses on enable,
three seconds dark on disable. Live switching retains the accepted brief pitch
change while latency adjusts. Pixel Bluetooth pairing and LHDC playback work,
but wireless playback on 0.1.28 still crackled and could become silent. See
[phone bridge](docs/PHONE-TRANSPORT.md) for that investigation and
[audio continuity](docs/AUDIO-CONTINUITY.md) for the accepted USB checks.

- 48 kHz stereo playback, with AAC-ELD preferred and AAC/SBC fallback.
- S16 USB compatibility plus IEEE float32 input; float processing quantizes once at the integer encoder boundary.
- Phone Bluetooth input with guarded LHDC v5 at 48 kHz, then classic aptX/SBC fallback.
- AACP microphone, opened by USB recording, with one owner at a time.
- Bluetooth HFP calls are disabled by default: SCO setup can stall the CYW43 controller. See [phone transport](docs/PHONE-TRANSPORT.md) for the experimental opt-in and evidence.
- USB phone calls use the existing duplex headset interfaces and 64 kHz microphone presentation clock.
- Phone or microphone decoding on core 1; Bluetooth, USB and playback encoding on core 0.
- Passive BLE discovery alongside stereo playback, with bounded Apple advertisement storage.
- Pro 3 case BLE battery fallback with separate provenance and per-component ages; hardware verification pending.
- 100 ms encoded startup silence for AAC-ELD, sent before accepting phone playback.
- USB playback separates its 20 ms steady reserve from 80 ms transient capacity and the 30 ms startup limit.
- Confirmed AVDTP suspension after one second without input; fresh input restarts and primes the stream.
- Stem media controls and volume routed to the active phone or USB input.
- NTPods USB controls and LibrePods controls over an encrypted, bonded phone connection.
- Model-gated AirPods settings, hearing profiles, saved hardware checks and app-owned ear detection.
- Selectable output codecs and supported bitrates, guarded Game Mode and performance readings.
- Nearby signal readings resolved against the selected pair's identity key.
- Two saved headset slots; reconnect uses the selected address and its bond.
- Solid LED while connected, including playback; pairing retains its blink.
- Bounded recovery, packet validation and USB suspend/disconnect cleanup.

NTPods uses the [USB control interface](docs/USB-CONTROL.md); LibrePods uses the
[phone control service](docs/CONTROL-EXTENSIONS.md). Both expose device settings
and readback without acquiring audio. USB uses Windows' built-in HID driver;
USB audio and microphone also work independently of the app. There are no
companion handoff commands. The physical single-press takeover request uses the
LibrePods AACP routing exchange. Short two-dongle handoff checks passed in both
directions; extended handoff and call testing remain pending.
Sensor streaming and case ringing are not implemented.
The serial port remains a diagnostic console.
Send `t` to read USB service and main-loop Bluetooth timing maxima, the current
phone decoder maximum and flash-cache counters. Repeated requests are coalesced;
counter collection does not acquire audio or run a sound-producing check.
Version 0.1.34 also accepts `bootsel` followed by Enter on the USB serial port.
From 0.1.46, the explicit command interrupts active audio and requests the ROM
USB bootloader without waiting for the host to stop. It does not erase pairing
or flash firmware. See [USB bootloader entry](docs/USB-CONTROL.md#usb-bootloader-entry)
for the session-fenced handshake and older-firmware limits.
LDAC is unsupported: the inherited encoder path could exceed the negotiated packet
limit, and AirPods do not need that codec.

## Bluetooth paths

The firmware links both Classic and BLE through one BTstack controller. The
[Pico SDK supports dual mode](https://www.raspberrypi.com/documentation/pico-sdk/networking.html#pico_btstack).

| Communication | Transport | Current implementation |
| --- | --- | --- |
| Playback and stem media controls | Classic AVDTP / AVRCP | Implemented |
| AirPods controls and HQ microphone | Classic AACP, PSM `0x1001` | Model-gated settings/readback; microphone requested by USB recording |
| Bluetooth phone calls | HFP HF / SCO or eSCO | Disabled by default because SCO setup can stall the controller; experimental CVSD bridge retained for development |
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
The connection pool supports the phone and AirPods ACL links plus one phone
SCO/eSCO voice link. Passive scanning uses no connection slot.

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

The standard firmware exposes float32 and S16 USB alternatives; the host chooses
one. A separate S16-only firmware download is unnecessary.

The SDK helper pins Pico SDK 2.3.1 and its submodules, checks original file hashes,
applies the reviewed patches, and verifies readback. Configuration and builds
reject missing patches or unknown patch states. The UF2 tool is pinned to
picotool 2.3.1. Use a separate SDK checkout rather than modifying a shared one.
Every firmware link runs the [real-codec memory check](tools/codec_probe/README.md)
against that ELF and its fingerprinted codec archives, including LHDC gain and
signal-error checks against the original synthetic PCM. UF2 publication waits
for successful validation; a failed or interrupted recheck removes stale flash
outputs. Tested allocations must leave a 64 KiB runtime reserve.

### Automated builds

[Firmware CI](https://github.com/Ripthulhu/airpods-dongle/actions/workflows/firmware.yml)
runs on every push and pull request, and can be started with **Run workflow**.
C/Python regression tests with address and undefined-behavior sanitizers must
pass before the Pico 2 W Release build starts. It uses the pinned SDK and
picotool, including the existing codec, memory and USB descriptor checks. Format
regression tests still exercise float32 and S16 input.

Open a successful run and download the **airpods-dongle-pico2w-float** artifact.
It contains the standard firmware with float32 and S16 USB input.

The ZIP contains the UF2 to flash, BIN/ELF/map files, the memory-check receipt,
source commit, compiler version and `SHA256SUMS`. Downloads are retained for
30 days and require GitHub sign-in. CI does not replace hardware listening tests.

## Use

The dongle starts reconnecting to its saved headset on power-up. Game Mode starts
off; double-press BOOTSEL to enable it for supported AirPods/codec combinations.
NTPods is optional: pairing, reconnects, audio, volume and stem
media keys run in firmware. A mode change lasts until the next power cycle.
Standalone Windows startup, playback and stems are verified. Earlier Pixel 10
Pro XL USB playback and stems worked, with intermittent crackle. Its current
firmware recheck and Symfonium USB-exclusive comparison are pending. On Windows,
0.1.24 completed a clean 349.888-second unpolled steady interval, and 0.1.25's
short S16/float microphone checks were accepted by the listener.

USB hosts need USB Audio Class 2 support and sufficient power. Playback is
48 kHz stereo using S16 or IEEE float32; recording advertises fixed 64 kHz,
16-bit mono. Windows 10 version 1703 or later supplies the audio driver. Phone support also depends
on USB host/audio support and the data cable or OTG adapter; it is not a promise
for every phone. See [Microsoft's class driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)
and [Android's USB audio baseline](https://source.android.com/docs/core/audio/usb).

Select **AirPods Dongle** as the USB playback and recording device. Hold BOOTSEL
for at least one second but less than three and release to pair AirPods or compatible
Apple audio already in pairing mode. Discovery skips unrelated speakers using the
Apple Device ID, with an AirPods-name or previously authenticated identity fallback
when Device ID is absent. A newly renamed pair without either identity must expose
its Device ID or an AirPods name to be discovered. Hold for at least three seconds to allow phone pairing for two
minutes, then select **AirPods Dongle** in the phone's Bluetooth settings. For
wireless use, power the Pico externally or leave its USB cable connected.
Connecting the phone's Bluetooth music profile removes the USB audio device;
disconnect the phone from the Bluetooth dongle to use USB audio again while the
AirPods are connected. With no AirPods connection, USB remains control-only.
Pausing music does not switch
back to USB. An existing USB recording session retains priority.
See [phone bridge](docs/PHONE-TRANSPORT.md) for details.
The selected saved headset reconnects automatically, with 1–8 seconds between
failed connection attempts. Pairing and codec negotiation suspend these retries.
A single press requests audio takeover from the other paired source, reconnecting
the selected headset if needed. Double-press BOOTSEL to toggle Game Mode live: enabling gives two
brief LED pulses; disabling turns the LED off for three seconds. It then returns
to the connected solid light. Hold BOOTSEL for AirPods pairing, as above.

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
python3 tools/test_volume_sdk.py --sdk .sdk/pico-sdk --sanitize
python3 tools/test_phone_codecs.py --sdk .sdk/pico-sdk --sanitize
python3 tools/lhdc_probe/run.py --sanitize
```

The volume SDK test exercises the patched AVRCP parser and registration handling,
including stale replies, status-only rejection and refresh cancellation. The
codec tests check LHDC/aptX/SBC, the actual worker, startup gate and BTstack
resampler. The LHDC probe compares decoded synthetic music with its original PCM
and checks malformed input and allocation failure. Both accept `--arm` instead
of `--sanitize` for an ARM32 functional check; it requires QEMU.

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

The inherited 250 MHz / 1.20 V overclock needs validation on each board. Radio
SPI is capped at 31.25 MHz independently of that CPU clock. Timing counters do
not establish acoustic latency or sustained codec headroom. Runtime heap margin
and microphone clock drift remain unmeasured. The microphone uses
the inherited 64 kHz USB presentation clock with a 48 kHz AAC coding configuration;
this does not establish 64 kHz acoustic bandwidth or calibrated recording speed.

Both cores have guarded 16 KiB stacks. The build measures codec allocation peaks
against the linked heap and requires at least 64 KiB left for runtime use.
The 0.1.25 float-capable link leaves 263,432 bytes of heap. The ARM probe peaks at
90,816 bytes for LHDC plus AAC-ELD, or 188,040 bytes for playback plus USB
microphone. The heavier case leaves 75,392 bytes before other runtime allocations,
above the required 65,536-byte reserve.
The [codec probe](tools/codec_probe/README.md) covers the real encoders, VBR frame
bounds and decoder lifecycles; emulation does not measure full runtime heap or
stack use. Performance readings expose queue fill, dropped/underflow frames,
encode time and allocator use for hardware checks.
Phone playback releases the idle microphone decoder before preparing its input
codec and 9,780-byte worker queue; USB recording waits for worker detachment and
frees both before allocating the microphone. Diagnostic history is limited to
2 KiB, with dropped-byte counts, to leave more memory for audio.
If microphone initialization fails, reopening recording makes one fresh attempt
after any failed worker has detached.

USB audio on Android needs an OTG/power and enumeration check. Wireless stereo
bridging also needs dual-link radio and latency checks. The experimental HFP
call bridge requires a working controller SCO implementation before call validation.

See [implementation notes](docs/IMPLEMENTATION.md) for the reviewed differences
and the boundaries that remain untested.

## Attribution

USBPods provides USB audio, codec and Bluetooth transport code. The two-way fork
adds the AACP microphone path derived from
[LibrePods PR 655](https://github.com/librepods-org/librepods/pull/655).
NTPods supplies the reference behavior for startup priming, guarded reconnect,
packet validation and microphone recovery. See [LICENSE](LICENSE),
[LICENSE.Apache-2.0](LICENSE.Apache-2.0), and the notices in `3rd-party/` and source files.
