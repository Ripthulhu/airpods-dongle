# ARM codec memory check

Every firmware link runs the built FDK and LHDC codecs with ARM32 pointers and newlib
allocation. It derives heap capacity from that ELF's `end` and `__StackLimit`,
then subtracts a required 64 KiB allowance for other runtime use. An archive hash
retained in flash binds each tested archive to the firmware. Missing tools,
unsupported layouts, mismatched inputs, failed checks or timeouts fail the build.

Requires the ARM build toolchain, `qemu-system-arm` and the matching Pico SDK
`picotool`. To repeat validation and regenerate the UF2:

```sh
python3 tools/codec_probe/run.py \
  --firmware build/airpods_dongle.elf \
  --library build/3rd-party/fdk-aac/libfdk-aac.a \
  --lhdc-library build/liblhdcv5.a \
  --picotool path/to/picotool --uf2-family rp2350-arm-s --uf2-abs-block
```

The CMake target `check_codec_memory` also repeats it. Generated files stay in
ignored `build/codec-probe`. Symbol inspection is bounded to ten seconds;
compilation and emulation each have a 60-second timeout. A failed or interrupted
recheck removes any previous passing receipt and the ELF's same-name UF2, BIN and
HEX files. After every check passes, picotool writes a temporary file which is
renamed to the final UF2. CMake disables the SDK's independent UF2 writer;
`check_codec_memory` uses the same validation and publication command.
An ordinary build also regenerates a missing UF2 after a failed manual recheck.
Its recovery rule reuses a just-completed link check only when the firmware,
archives, validation inputs and UF2 still match the receipt.
`memory-check.json` records those hashes, capacity, reserve, exact worker size,
per-mode allocator peaks, remaining space and both PCM quality results.

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
- A separate process keeps real LHDC decoding, stereo ELD-256 and the phone worker's
  exact linked allocation live together. Two allocation orders decode 64 synthetic
  music frames and encode 32 ELD frames, then verify all heap allocations are freed.
  Each 32-frame cycle independently compares 819 stereo sample pairs with the
  original source PCM, requiring SNR above 30 dB and gain within 0.5% on each channel.
  This rejects silence, channel swaps and scaling errors without assuming identical
  floating-point output across compilers. The fixture and reference PCM are link
  dependencies, so changing either reruns the gate against the exact LHDC archive.
  The worker size is read from a retained value in that ELF, rather than copied
  from a source constant. Each mode starts with a fresh allocator arena.

With GCC 13.2.1, Pico SDK 2.3.1 and firmware 0.1.10, live stereo/microphone allocations total
186,872 bytes; the tested allocator peak is 188,040 bytes. LHDC/ELD/worker live
allocations total 85,512 bytes and peak at 90,816 bytes. The linked heap is
386,008 bytes, leaving 197,968 bytes at the heavier peak before other runtime allocations.
The 64 KiB reserve is a policy allowance, not a measured application requirement.
The CVSD phone-call path retains the same stereo playback/mono microphone codec
pair; its static HFP and PCM state is included in the linked heap capacity.
The memory gate does not emulate call transport, ownership or controller timing.

Stack markers start 128 bytes below the helper's stack pointer. Tested decoder
and stereo encoder depths are 7,420 and 7,332 bytes below the marker. The artificial
LHDC-to-ELD callback reaches 11,244 bytes; production splits decoding and encoding
across the two cores. These exclude
the caller's existing frame and firmware interrupt nesting; they do not prove
a bound for every input or replace firmware stack guards.

QEMU 8.2's Cortex-M33 model lacks an FPU. The harness runs the same M33-compiled
fixed-point library on its Cortex-M7 model, which supports these instructions and
newlib's FP register transfers. Allocation layouts remain ARM32. No Pico,
Bluetooth or USB device is emulated, and the generated microphone fixture is not
an AirPods capture. This does not measure board timing, concurrency, power,
radio behavior or actual runtime memory use.
