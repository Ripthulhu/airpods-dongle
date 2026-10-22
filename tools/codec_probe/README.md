# ARM codec memory check

Every firmware link runs the built FDK codec with ARM32 pointers and newlib
allocation. It derives heap capacity from that ELF's `end` and `__StackLimit`,
then subtracts a required 64 KiB allowance for other runtime use. An archive hash
retained in flash binds the tested library to the firmware. Missing tools,
unsupported layouts, mismatched inputs, failed checks or timeouts fail the build.

Requires the ARM build toolchain and `qemu-system-arm`. To repeat the check:

```sh
python3 tools/codec_probe/run.py \
  --firmware build/airpods_dongle.elf \
  --library build/3rd-party/fdk-aac/libfdk-aac.a
```

The CMake target `check_codec_memory` also repeats it. Generated files stay in
ignored `build/codec-probe`. Symbol inspection is bounded to ten seconds;
compilation and emulation each have a 60-second timeout. A failed or interrupted
recheck removes any previous passing receipt. `memory-check.json` records input
hashes, capacity, reserve, allocator peak and remaining space.

The check covers:

- Stereo ELD at 48 kHz/256 kbps together with mono microphone decoding. Sixteen
  generated 480-sample frames with ASC `F8 E6 30 00` retain checksum `4105af5d`.
- 960 encoded frames across ten AAC/AAC-ELD bitrate configurations, compared with
  fixed hashes from the original library.
- Unsupported configurations, truncated/malformed input, reset recovery, limiter
  operation and output canaries.
- Eight allocation lifecycles each for ELD-256, LC-128 and LC-220, including both
  allocation orders and recreating playback while retaining the microphone decoder.
- 480 ELD VBR frames across five bitrate ceilings with the microphone retained;
  every access unit stays within its negotiated byte limit.

With GCC 13.2.1 and firmware 0.1.8, live stereo/microphone allocations total
186,872 bytes; the tested allocator peak is 188,040 bytes. The linked heap is
386,800 bytes, leaving 198,760 bytes at that peak before other runtime allocations.
The 64 KiB reserve is a policy allowance, not a measured application requirement.

Stack markers start 128 bytes below the helper's stack pointer. Tested decoder
and stereo encoder depths are 7,420 and 7,332 bytes below the marker. These exclude
the caller's existing frame and firmware interrupt nesting; they do not prove
a bound for every input or replace firmware stack guards.

QEMU 8.2's Cortex-M33 model lacks an FPU. The harness runs the same M33-compiled
fixed-point library on its Cortex-M7 model, which supports these instructions and
newlib's FP register transfers. Allocation layouts remain ARM32. No Pico,
Bluetooth or USB device is emulated, and the generated microphone fixture is not
an AirPods capture. This does not measure board timing, concurrency, power,
radio behavior or actual runtime memory use.
