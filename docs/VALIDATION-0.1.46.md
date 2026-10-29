# Active-audio update validation — 0.1.46

Both lab Pico 2 W boards were updated from 0.1.44 to the standard float-capable
0.1.46 image. Each exact board identity, installed version and 4 MiB flash size
was checked. Complete private backups, verified programming and byte-for-byte
comparison of the final 64 KiB pairing area passed before reboot. Both boards
restarted. The old phone firmware required physical BOOTSEL for this installation.

## Live phone check

The listener confirmed music playing through the Pixel's USB dongle with phone
Bluetooth off. Immediately before the runtime request, the selected board had
report-2 state flags 71: AirPods signaling, AACP and playback active, plus USB
streaming. The encoder advanced by 37 units during the bounded preflight. The
runtime capability field was 511, including active-update capability 256.

The host then sent the exact CDC `bootsel` command with DTR asserted while the
audio route remained active. The same serial reappeared as `2E8A:000F` in ROM.
Picotool confirmed chip identity, firmware 0.1.46 and BOOTSEL mode. The host
restarted the installed firmware without programming another image; runtime
controls returned. The listener confirmed music returned in both ears.

This verifies active USB playback does not block entry into the update mode.
It is not a claim of continuous audio during reboot. No volume command, player
pause, route-forcing setting or pairing reset was needed. The Android automatic
USB routing preference remained `null` across installation. Detailed packet
logging was not enabled and the diagnostic handles were closed afterward.

## Implementation and offline checks

The pinned Pico SDK 2.3.1 implements RP2350 `reset_usb_boot` through a whole-chip
watchdog reset into ROM, stopping both application cores. Explicit maintenance
entry therefore no longer checks audio-idle state. It retains the exact command,
same DTR/session fence, 20–100 ms bounded reply drain and final IRQ-safe session
check outside the Bluetooth lock. The activity predicate remains in idle-power
pacing. No asynchronous mic/call teardown or USB personality switch is added.

Production-source ASan/UBSan tests cover eight active owners individually and
together, ongoing USB activity, capture/call/transition starting after acceptance,
no command/no reset, duplicate requests, bounded drain, timer wrap and DTR/session
loss at both stages. Restoring either old audio guard fails the new assertions.
Parser tests retain fragmented command, malformed input and disconnect coverage.
Microphone and call activity were simulated; the live check used USB playback.

All 62 C sanitizer suites and 63 Python tests passed (one optional test skipped).
Real-SDK volume and phone-codec tests, LHDC safety, the single standard Release
build, emitted USB descriptors and linked-memory checks passed. Heap capacity is
254,312 bytes; the measured worst codec allocation leaves 66,272 bytes against
the unchanged 65,536-byte reserve. The image includes float32 and S16 input.

Evidence in project `airpods-pico-bringup-20261027`:

- `build-receipt-0.1.46-r2.json`, `logs-0.1.46-r2/` and
  `release-0.1.46-float-r1/` (the executed build's release-layout suffix).
- Source archive SHA-256:
  `340d200e9550e8c21942929eada2ebebce4e882e2b76cc6dffa5ec378cfcfd95`.
- Standard UF2 SHA-256:
  `d048aefa0a0da5bf6203a724346152ebfe940593814bb57da25c88b5b73f8e20`.
- Per-board deployment receipts, the live active-update JSON/log, pinned-SDK
  review and two old-guard negative controls. Full flash and pairing backups stay
  private and are excluded from public reports.

All 749 source/configuration/test hashes match the checked build snapshot.
Final deployment documentation was added after the build. No separate S16-only
image was built or published for this version.

## Limits

This deliberately interrupts the dongle's current audio connection. The external
Bluetooth link may linger until supervision expires or radio initialization;
graceful Bluetooth teardown and absence of reconnect sounds are not promised.
The updater must still verify the selected board in ROM before flashing, validate
the image and preserve pairing storage. Entering ROM alone does not erase flash.

The 0.1.45 volume-authority API is included, but its Android host bridge and
maximum-volume-cue behavior have not been accepted on hardware. This update test
does not resolve that remaining host integration work or establish power savings.
