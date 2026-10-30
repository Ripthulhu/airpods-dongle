# LED startup correction - 30 October 2026

The two builds below record the initial LED deployment. Both dongles now use the
[shared 0.1.57-r6 release](UNIFIED-FIRMWARE-20261030.md); 0.1.54-r7 is a historical
PC rollback baseline.

Startup directly drove the CYW43 LED high before an AirPods connection existed.
A failed connection attempt left that write in effect. Startup now calls the
existing `set_led_mode_off()` immediately after successful CYW43 initialization;
the later unconditional high write is removed. The normal connection, disconnect,
pairing and Game Mode feedback handlers are unchanged.

| Device build | Matching source commit | Release |
|---|---|---|
| PC 0.1.54-r7, 20 kHz | `0c3f36ac895ee80946cb48fc4a53b91c1a852512` | [PC firmware and source](https://github.com/Ripthulhu/airpods-dongle/releases/tag/v0.1.54-r7-bw20000) |
| Phone 0.1.57-r6, 20 kHz | `439ce4d95d6b0957474fd3a4a29434dca06af05a` | [Phone firmware and source](https://github.com/Ripthulhu/airpods-dongle/releases/tag/v0.1.57-r6-bw20000) |

Each release changes only `src/main.c` relative to its exact previous good build:
one line added, two removed. Audio, volume, codec and radio firmware source are
unchanged. Both builds retain PCM32/float32 USB input and 20 kHz AAC-ELD bandwidth.
The prior [PC r6](https://github.com/Ripthulhu/airpods-dongle/releases/tag/v0.1.54-r6-bw20000)
and [phone r5](https://github.com/Ripthulhu/airpods-dongle/releases/tag/v0.1.57-r5-bw20000)
builds remain available separately.

## Checks and installation

Both builds passed the full sanitizer regression suite, existing LED timer tests,
real codec transition checks, SDK volume/phone codec checks, LHDC safety checks,
firmware build and linked memory/quality gates. The phone build also passed its
CYW43 transport regression. Every committed source file was compared with its
frozen manifest; every release output was checked against its build receipt.

Both identified dongles were idle before installation. Their complete flash was
backed up privately, each new image was written and verified, and the final 64 KiB
of pairing/settings storage was compared byte for byte with the backup. Both
returned to their production USB interface and answered runtime diagnostics with
no active audio, microphone or call. LibrePods was reopened on the Pixel.

The user confirmed the LED stays off at boot and turns off correctly when AirPods
disconnect. Existing LED tests also cover pairing blink, connected indication and
cancellation on disconnect. No new listening test was run for this LED-only change.

The [controller research checkpoint](CYW43439-RESEARCH-CHECKPOINT-20261030.md)
remains paused. These builds contain the normal Pi radio firmware, not the failed
`.0076` hybrid. No experimental controller probe remains running.
