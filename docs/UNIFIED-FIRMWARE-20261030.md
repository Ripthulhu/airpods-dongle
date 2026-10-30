# One firmware for both dongles

The current shared release is [0.1.57-r6-bw20000](https://github.com/Ripthulhu/airpods-dongle/releases/tag/v0.1.57-r6-bw20000).
Use its single `airpods_dongle.uf2` for either Pico 2 W. USB audio and Bluetooth
relay are runtime paths in the same application, not different device builds.

Consolidation reuses the exact image already installed on the phone dongle and
installs it on the PC dongle too. There is no new encoder, volume, gain, buffer,
latency or controller-firmware change for this release promotion. It retains the
LED correction, PCM32/float32 USB alternatives, native hardware volume and 20 kHz
AAC-ELD bandwidth, plus the newer CYW43 transport fixes.

| Identity | Value |
|---|---|
| Source tag | `v0.1.57-r6-bw20000` |
| Exact firmware source commit | `439ce4d95d6b0957474fd3a4a29434dca06af05a` |
| UF2 SHA-256 | `ff8d682d1f8168e3e0473f37551d23b03b2c55f131df9efbe79b12e7f70cb605` |
| Frozen source archive SHA-256 | `d5bab74c0d8875234198b785c4873f307594905e33063f90c4465b2f523e760c` |

Each dongle keeps its own identity, pairing records and settings. Only the firmware
region is updated; full-device backups must not be copied between boards. The PC
upgrade preserved its final 64 KiB of pairing/settings storage byte for byte. The
phone already had this exact image and did not need another restart or reflash.
The PC returned to its production USB interface and reconnected to the AirPods.

## Validation

The existing immutable build passed sanitizer regressions (including LED and
volume fixtures), real codec transition checks, CYW43 transport and SDK volume/
phone-codec checks, LHDC safety checks, and the linked memory/quality gate. Its
release assets and frozen source hashes were rechecked before PC deployment.

PC listening and live volume acceptance are recorded in `unified-validation.json`
on the release. That record distinguishes this hardware check from the original
offline build receipt; neither the build receipt nor the original source tag is
rewritten.

The user accepted familiar music, five-second pause/resume, stem down/up and PC
volume controls. Playback used USB alternate 2 (float32). In an 8.2-second steady
interval, 820 units encoded with zero new active sample drops, active zero fills,
USB bad packets/short reads, discarded debt, timeline recoveries or controller
flushes. The bounded 60-second capture also retained zero active drops/zero fills.
Native gain readback was known, with no pending/in-flight volume request. The
host's final user-selected volume was left untouched by the updater.

This was a short playback/volume check, not an extended soak or a new acoustic
latency measurement. No additional phone listening test was needed to replace
its firmware: its previously verified image was left unchanged.

The older [PC 0.1.54-r7 release](https://github.com/Ripthulhu/airpods-dongle/releases/tag/v0.1.54-r7-bw20000)
is retained as a historical rollback, not a second current firmware. The
[initial LED deployment](LED-STARTUP-FIX-20261030.md) documents the earlier split.
The [controller investigation](CYW43439-RESEARCH-CHECKPOINT-20261030.md) remains
paused; no experimental `.0076` controller image is installed.
