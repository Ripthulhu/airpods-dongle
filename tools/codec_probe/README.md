# ARM codec probe

Runs the built FDK codec with 32-bit ARM pointers and newlib allocation. It checks
real stereo AAC-ELD encoding, mono microphone-shaped decoding, codec heap use and
stack writes. No Pico, Bluetooth or USB device is emulated.

Requires the build toolchain and `qemu-system-arm`. Run on Linux/WSL after building:

```sh
arm-none-eabi-nm build/airpods_dongle.elf | rg ' (__StackLimit|end)$'
python3 tools/codec_probe/run.py \
  --library build/3rd-party/fdk-aac/libfdk-aac.a \
  --heap-bytes 392512
```

Calculate `--heap-bytes` as `__StackLimit - end` from that exact firmware ELF;
392512 is an example build value. Generated files stay in ignored `build/codec-probe`.
The runner bounds compilation to 60 seconds and emulation to 40 seconds.

The fixture generates 16 mono 480-sample AAC-ELD frames at 48 kHz, 64 kbps, with
ASC `F8 E6 30 00`. It closes that fixture encoder, keeps the production-style
stereo 256 kbps encoder open and decodes all 16 mono frames. The encoded data and
decoded samples must retain checksum `4105af5d`. The fixture's preparation encoder
is not part of the measured simultaneous encoder/decoder heap use.

With GCC 13.2.1 and the pinned library, before and after the coupling-element
parser stack fix, live codec allocations total 379864 bytes: 121696 for the stereo
encoder and 258168 more for the configured mono decoder. Allocator bookkeeping
and fragmentation use additional space; this is not total firmware memory use.

The stack marker starts 128 bytes below the probe helper's stack pointer. Reported
depths exclude that gap, the caller's existing frame and firmware interrupt nesting.
The tested decoder path changed from 41972 bytes below the marker to 7420 bytes;
the stereo encoder path touches 7228 bytes. These observations do not
prove a bound for every input or replace firmware stack guards.

The installed QEMU 8.2 Cortex-M33 model does not expose an FPU. The harness runs
the same Cortex-M33-compiled fixed-point codec on its Cortex-M7 model, which can
execute these instructions and newlib's FP register transfers. Pointer sizes and
allocation layouts remain ARM32. This is a functional and memory check, never an
RP2350 timing, concurrency, power or radio measurement. The generated fixture is
not a captured AirPods microphone stream.
