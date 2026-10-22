# LHDC decoder checks

From Linux/WSL:

```sh
python3 tools/lhdc_probe/run.py --sanitize --output build/lhdc-probe/native
python3 tools/lhdc_probe/run.py --arm --output build/lhdc-probe/arm
```

The native check uses ASan, UBSan and float-to-integer overflow checks. Both runs
decode the same 32-frame AOSP-verified complex fixture and compare unscaled source
PCM after its measured 168-sample transform delay. Gain must stay within 0.5% and
SNR above 30 dB without fitting output gain. Tests also cover malformed packet
tails, every one-bit mutation of a fixture frame, 20,000 deterministic malformed
frames up to 1,024 bytes, checked history-window bounds, unsupported formats,
grouped frames, allocation failures, allocation-free decode and complete cleanup.

The ARM check uses Cortex-M33 DSP instructions, ARM32 pointers and newlib. QEMU's
M33 model lacks an FPU, so the same code runs on its M7 model, as in the existing
codec probe. It measures requested heap, newlib's arena and stack writes below a
marker 128 bytes below the probe's stack pointer. These exclude the firmware
caller's stack, IRQ nesting and other runtime allocations. They are not physical
core 1 timing or full firmware memory measurements.

The tool saves bounded-run logs, source fingerprints and a receipt beside its
executable. A failed run removes an earlier receipt. Production core 1 timing
and the combined linked-firmware memory check remain separate gates.
