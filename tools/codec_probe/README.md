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
The firmware and probe both require `FDK_DONGLE_PCM32=1`: `INT_PCM` and decoder
output are Q31 throughout FDK. ELD input is Q31; AAC-LC retains its S16 input
through FDK's explicit S16-to-Q31 frontend. The probe uses the same maximum
4096-byte encoder input union, rather than adding a second wide music buffer.
`--eld-cbr-bandwidth` accepts 0, 16000, 18000 or 20000 Hz and must exactly match
the word retained in that ELF. The default is 20000; changing this option never
changes the reserve or packet limits.

The check covers:

- Stereo ELD at 48 kHz/256 kbps together with mono microphone decoding. Sixteen
  generated 480-sample frames with ASC `F8 E6 30 00` retain checksum `4105af5d`.
  That historical combined checksum includes the original automatic-bandwidth
  mono fixture, one bandwidth-0 stereo access unit and projected microphone PCM.
  Candidate-bandwidth stereo/microphone memory is checked separately below.
- 960 encoded frames across ten AAC/AAC-ELD bitrate configurations, compared with
  fixed hashes from the original library. This historical matrix explicitly uses
  bandwidth 0 for ELD, independently of the candidate's configured bandwidth.
- Another 480 CBR ELD frames across all five bitrate profiles use the candidate's
  exact bandwidth with the microphone decoder retained. Format, consumed samples,
  CBR packet ceilings and cleanup remain mandatory, with per-profile heap reports.
  The runner requires both this coverage and the historical hash matrix.
- Legacy AAC-LC at 192 and 256 kbps runs 192 more 1024-sample frames with S16
  input elements and the microphone decoder retained in the global Q31 library.
  The original 1024-byte output bound and allocation cleanup remain mandatory.
- Unsupported configurations, truncated/malformed input, reset recovery, limiter
  operation and output canaries.
- Public encoder boundaries before first input allocation: Info/Close safety,
  suppressed initialization via CONTROL_STATE, invalid ELD frame rejection without
  allocation, and successful valid initialization afterward. Existing close/open
  lifecycle checks cover production codec switches; helper tests cover denied
  buffer growth and retry without changing the memory gate.
- Complete Q31 decoder writes, nonzero low-order output bits, and 32-bit canaries.
  Existing microphone golden checks compare the historical rounded S16 projection
  of that output; generated ELD fixtures explicitly multiply their S16 references
  by 65536, without shifting negative signed values.
  Real linked decoder workspace getters must match 4096 spectral payload bytes
  and 3840 time-domain payload bytes plus FDK's pointer/alignment overhead:
  `ALGN_SIZE_EXTRES(payload + ALIGNMENT_DEFAULT + sizeof(void *))`.
  With the pinned ARM32 alignment, required sizes are 4112 and 3856 bytes.
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

Historical S16 measurements with GCC 13.2.1, Pico SDK 2.3.1 and firmware 0.1.10: live stereo/microphone allocations total
186,872 bytes; the tested allocator peak is 188,040 bytes. LHDC/ELD/worker live
allocations total 85,512 bytes and peak at 90,816 bytes. The linked heap is
386,008 bytes, leaving 197,968 bytes at the heavier peak before other runtime allocations.
The 64 KiB reserve is a policy allowance, not a measured application requirement.
The CVSD phone-call path retains the same stereo playback/mono microphone codec
pair; its static HFP and PCM state is included in the linked heap capacity.
The memory gate does not emulate call transport, ownership or controller timing.
These historical sizes are not a measurement of the Q31 candidate. Its exact
linked firmware must pass the same allocator/reserve gate before publication.
If `_sbrk` denies a request, 12 bytes of probe-only diagnostic state retain the
last request, current break offset and denial count without allocating or logging.
The normal failure handler then prints those values and the unchanged budget.
This diagnostic BSS is separate from the firmware-derived codec heap budget.

`tools/test_adaptive_codec.py` separately runs the real encoder frontend with a
GNU link wrapper around the archive's Q31 deinterleave call. Two inputs with
identical upper 16 bits and different lower bits must remain distinct in the
actual `aacEncEncode` input buffer. Deliberately truncating that buffer to the
S16 grid must be rejected. This diagnostic instrumentation stays outside the ARM
memory probe and production firmware; it is not an audible codec quality claim.
The focused native frontend test also widens every possible S16 value through
the actual FDK function, checking exact multiplication and planar stride guards.

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
