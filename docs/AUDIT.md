# Phone bridge and NTPods USB audit

What this repo does: The Pico receives aptX/SBC from a phone or PCM audio over
USB, then encodes audio for the AirPods. NTPods uses standard USB HID for dongle
controls. The assumed load is one phone, one AirPods pair, stereo playback,
optional USB HQ microphone and passive BLE; USB audio takes priority.

The audit covers baseline `649d8e0`, the cross-core worker, USB controls,
hearing/diagnostic transport and optional NTPods driver installation.
All six findings below are fixed.

## Must fix — completed

1. **Invalid USB commands broke later status reads** (`src/tinyusb/dongle_control.c:68`).
   - **What this is:** The app reads a status report containing the last command result.
   - **Problem:** An unknown command code was copied into a field the host rejects.
   - **Fix:** Return a valid error receipt; test that a later valid request works.
   - **If skipped:** One malformed writer could hide controls until reset.

2. **USB discovery delays could pull audio onto the PC adapter** (`ntpods/windows/daemon/src/dongle.rs:29`).
   - **What this is:** NTPods suppresses automatic direct Bluetooth connections while a dongle is present.
   - **Problem:** Slow discovery or a failed read could look like an absent dongle.
   - **Fix:** Wait for initial discovery and retain known presence through read failures. Explicit native Connect remains available.
   - **If skipped:** A control-channel delay could interrupt dongle playback.

3. **Pairing could report success without starting** (`src/tinyusb/dongle_control.c:81`).
   - **What this is:** The firmware reports whether it accepted a pairing request.
   - **Problem:** Unavailable phone storage, failed slot clearing or failed inquiry startup did not reach the receipt.
   - **Fix:** Propagate those failures. Acceptance still means local acceptance, not a completed bond or device readback.
   - **If skipped:** Users wait for pairing that never started.

4. **App-only setup still depended on test-signed drivers** (`ntpods/windows/installer/msi/NTPods.wxs:109`).
   - **What this is:** The same installer supports PC Bluetooth and the USB dongle.
   - **Problem:** Setup required Test Mode and changed Bluetooth configuration even for an app-only installation.
   - **Fix:** Default fresh installs to app-only; make Direct Bluetooth explicit. Keep driver signing, binding, helper tasks and identity changes in that branch, preserving existing ownership through upgrades.
   - **If skipped:** Dongle users still need the custom driver setup the dongle is meant to avoid.

## Should fix — completed

5. **Phone decoding left the second core idle** (`src/btstack/phone_worker.c:148`).
   - **What this is:** Phone playback decodes the incoming codec and encodes a separate AirPods stream.
   - **Problem:** Both operations ran on core 0 alongside Bluetooth and USB.
   - **Fix:** Decode complete packets on core 1 through bounded queues. Generation checks and detach acknowledgement prevent stale output and freeing a running decoder.
   - **If skipped:** Decode bursts compete directly with radio and USB handling.

6. **Debug history used scarce audio memory** (`src/tinyusb/debug_cdc.c:23`).
   - **What this is:** A ring buffer retains console output until USB can read it.
   - **Problem:** Its 8 KiB reservation reduced the small margin after stereo encoder and microphone initialization.
   - **Fix:** Reduce it to 2 KiB, recovering 6 KiB while retaining overflow counts and the existing drain limit.
   - **If skipped:** Old log messages reserve memory that live audio may need.

## Remaining checks

- Pico dual-link scheduling, CPU utilization, IRQ duration, power/overclock
  stability, heap/stack peaks and acoustic delay require hardware.
- AirPods discovery inherits the source firmware's audio-device class filter;
  keep unrelated headsets/speakers out of pairing mode. It is not authenticated
  AirPods model identification. The selected saved address and encrypted phone
  bond are checked on subsequent connections.
- The inherited microphone has a 64 kHz USB presentation clock and 48 kHz AAC
  coding configuration. Actual capture duration/pitch and clock drift remain
  unverified.
- Test fresh app-only installation on a Windows machine without Test Mode,
  actual USB enumeration/unplug/reset, phone-to-USB takeover and microphone
  recovery. No live installation or driver changes were made for this audit.
- Verify hearing-mode reports, complete profile preservation and diagnostic
  readback through USB. Mode requests remain unconfirmed if the AirPods do not
  send a fresh report; they may require a reconnect. The firmware's remaining
  198,752 bytes at the emulated codec peak still need a hardware peak measurement
  before release.

## Memory fixes

All six memory findings are addressed:

- A failed microphone initialization no longer blocks future recording opens.
  Each open gets one attempt after worker detachment; cancellation clears it.
- Stereo encoder scratch no longer reserves eight channels: 57,312 bytes saved.
- The retained limiter is sized for mono/48 kHz: 46,080 bytes saved.
- Raw microphone transport uses 512 bytes with a 255-byte input guard:
  32,256 bytes saved.
- The guarded mono decoder uses an 8 KiB time workspace: 57,344 bytes saved.
- Every linked firmware runs the real ARM codec check against its own heap and
  fingerprinted archive, with a required 64 KiB reserve for other runtime use.

Combined live codec allocation falls from 379,864 to 186,872 bytes. Playback and
microphone queues, 100 ms priming and both 16 KiB stacks are preserved. Unsupported
formats fail before reduced buffers are used. Regression checks cover failed
initialization, reopen, cancellation, malformed packets, active reconfiguration,
limiter operation and repeated codec lifecycles. Actual board memory and radio
measurements remain pending.

## Hearing and diagnostic checks

The settings path uses bounded, explicitly committed transactions and closes
ATT after timeout or cancellation. Tests cover fragmented writes, repeated
commits, stale generations, USB loss, malformed packets and response pages.
The app pins the original USB handle and keeps native and dongle state separate.

Review corrected overlapping health/control requests, a timing-based freshness
check that could reject fast reports, and disappearing progress during case
disconnection. Mode confirmation now requires a new device-report revision;
profile writes preserve the full record and require readback. Self-checks need
explicit confirmation, known model/firmware and fresh in-case reports. Firmware
checks case and audio state again immediately before sending the request.

## Evidence

The baseline was checked against the pinned BTstack implementation, including
AVDTP event layouts, deferred Start acceptance, codec bounds, AVRCP routing and
phone encryption/bond requirements. No further reproducible protocol or security
defect was found in that baseline. Follow-up regression coverage includes malformed
commands, stale targets, replayed sequences, USB loss, queue bounds, partial
decode failures and concurrent worker teardown.

Offline firmware checks passed: 28 native C test programs with ASan/UBSan, 23 Python tests,
real aptX/SBC codec, resampler and selected-pair AES checks on host and ARM emulation, and the ARM
Release firmware build with its linked-memory check. The profiled FDK library
also passed native ASan/UBSan/leak checks; signed-shift checks are excluded for
its fixed-point arithmetic. A non-profile smoke test retains eight-channel
encoder construction, ADTS and stereo AAC-LC decoding. NTPods passed 740 daemon, 92 playback and 8 diagnostic
tests, Rust/C# IPC fixtures, four-language checks and its WinUI Release build.
Eight existing hardware-dependent tests remain ignored. Installer feature,
upgrade-ownership and rollback checks pass on PowerShell 5.1 and 7.

ThreadSanitizer cannot run on this WSL host due to its memory mapping limitation;
the threaded worker tests passed ASan/UBSan. Emulator timings are not Pico performance measurements.
Verdict: The reported defects are fixed, both cores have useful work, and the
USB path supports app-only installation. Hardware validation remains the release gate.

Not checked: live Pico behavior, a fresh Windows installation without Test Mode,
Android OTG and phone calls. This was not a full audit of unrelated NTPods features
or every third-party codec routine.
