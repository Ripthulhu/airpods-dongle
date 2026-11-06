# CYW43439 .0076 build and validation

The normal CI release enables this frozen .0076 path for the single unified PC
and phone firmware. The legacy `AIRPODS_EXPERIMENTAL_RADIO0076` option name is
retained; its local CMake default remains OFF, so a local reproduction must pass
`-DAIRPODS_EXPERIMENTAL_RADIO0076=ON` explicitly. Use a separate build directory
when changing that option. CI packaging validates the selected build flags and
linked radio identity. Nothing in this directory installs firmware on a device.

The original UART .0076 image is from Infineon's `ifx-linux-bluetooth` repository;
[`SOURCES.json`](../../SOURCES.json) pins its commit, path and hash separately from
the modified Pico container. The exact vendor [EULA](LICENSE.vendor.txt) is
retained. Its section 2(b) distinguishes source-provided modification rights
from binary firmware distribution on Infineon hardware, and section 4 limits
modification and reverse engineering except where otherwise permitted. These
terms remain specific to the vendor input; its attribution and frozen hashes do
not grant additional rights for the modified container or relicense it as GPL.

The container and native shared-SCO adapter are copied byte-for-byte from the
qualified `cyw43439-sco-shared-bridge-v6-20261101` test bundle. `frozen.json` pins
the reused data and bounded readback helper. The complete native C, Thumb assembly,
linker layout and original hook manifest are in `native/`. Rebuild with the pinned
Android NDK **30.0.14904198**, LLVM revision
`9f872551d3c681d06fd303b36f16ed5c274735eb` (Clang/LLD 21.0.0):

```sh
python3 tools/build_radio0076_native.py --toolchain /path/to/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
```

The build uses temporary files, zero-fills the reserved state sections and requires
the qualified 4,788-byte SHA-256
`c63e482e377d295ff3bb822345a5ac9cf883094d9671563f5f1ad3211394e54e`.
It checks all four rebuilt hook branches and state addresses against the committed
header. It never rewrites or silently blesses a differing blob. The hook manifest
contains only the reviewed ROM guard words; no private ROM dump is needed.

`tools/check_radio0076.py --elf ...`
also proves that the linked controller container, native adapter and all five
readback spans match the frozen source. It refuses a stock SDK firmware fallback.

Startup order:

1. Arm a six-second startup deadline before CYW43 initialization.
2. Check the downloaded container's length/CRC and all five controller readback
   spans before the loader's final callback executes.
3. Install the guarded early mode-2/TCA bootstrap and verify its retirement,
   RF inputs, signed configuration completion and calibration shape.
4. Apply only the previously qualified masked calibration values in unsigned RAM.
5. Install and read back the unchanged native SCO adapter and its four hooks,
   publishing enable bits only after every write verifies.
6. After HCI Reset reaches WORKING, verify the native hook registry and configure
   the shared CPU-copy route while all SCO endpoints are idle. This event handler
   is registered before every application profile handler.

Any failure stops startup with a named error. There is no old-firmware fallback,
automatic re-pairing, controller vendor-clock command, RF experiment or per-packet
host diagnostic capture. AAC/ELD, Game Mode, USB formats, hardware volume, flash
storage layout and existing SDK transport/backpressure patches are unchanged.

The test harness used diagnostic fatal/allocator hooks in slots 39/40. This build
omits those hooks and leaves their enable bits clear, reserves their slot numbers,
and keeps the four native adapter hooks in slots 41–44. The resulting enable mask
is `1c001e7f`; the next slot remains 45. This small policy difference and the
production threaded transport do not inherit full hardware qualification from
the earlier test bundle. A passing CI build does not establish listening,
latency, long-run radio behavior or two-way call acceptance.

`python3 tools/test_radio0076.py` runs bounded host tests with ASan/UBSan for
readback, bootstrap identity/publication order, calibration guards/masked writes,
native installation/publication order, idle-only configuration, fail-closed
download/ready ordering and HCI event gating. Existing regression, SDK transport,
volume and codec/resource checks remain required.
CI runs these tests and the native rebuild, then explicitly enables this path
for the normal `airpods-dongle-pico2w` artifact and release download. That
same image serves PC USB, phone USB and Bluetooth relay; no second legacy-radio
image is published. Preserve the SDK, compiler, build-option, source and memory
receipts when reproducing a release. The local CMake default and hardware
qualification boundary remain separate from CI's release selection.
It also runs the [native instruction-level behavior proofs](../../tests/radio0076/native/README.md)
through `tools/test_radio0076_native.py`: 963 credit cases, 23 inbound groups,
17 outbound groups and four reset/glue groups, using production sources and a
bounded code-only ROM fixture.

This adapter is tied to the exact frozen .0076 container and reviewed ROM words.
It supports only the qualified 60-byte CVSD host / 120-byte native SCO geometry.
Bluetooth calls remain disabled by default. A media listening/latency/stability
comparison is separate from CI; synthetic SCO success does not establish call
audio or replace those media checks.

The application still uses its existing flash bond database and can update
AirPods-side settings. Before installation, verify the board identity and retain
a backup/restoration plan for flash pairings and actual receiver settings. The
UF2 is flash firmware, not a RAM-only test; publishing a normal release does not
expand the hardware results recorded above.
