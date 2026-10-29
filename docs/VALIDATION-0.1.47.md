# Volume and Game Mode candidate — 0.1.47

## Volume

A single downward stem swipe on Windows with 0.1.46 changed the AirPods from
121 to 113 and delivered one USB volume-down key. Windows sent a USB volume
request that still mapped to AVRCP 121. The acknowledgement caused the firmware
to publish its rounded equivalent back to Windows: -2.36328125 dB, displayed as
85.04%. Repeating a small step could therefore return to the same value.

The firmware now preserves the exact USB request when its mapped AVRCP value is
confirmed. No-op writes and later identical reports obey the same rule. A real
device clamp still updates the host. Regression tests exercise repeated fractional
steps across AVRCP boundaries in both directions; the test fails without the fix.

A separate reproduced ordering bug allowed an outstanding native INTERIM report
to overwrite a newer accepted SET. The acknowledged registration generation now
fences that old report. Later CHANGED notifications can renew normally. Tests
cover initial and renewed subscriptions, leased control, and generation wrap.

The near-maximum tone remains a separate limitation: a physical stem step can
reach the AirPods' maximum before the host slider reaches its maximum. These
changes do not claim to eliminate that tone.

## Game Mode

The captured macOS transition uses a 60-ms automatic flush timeout (96 Bluetooth
slots) in Game Mode and restores 200 ms (320 slots) afterward. The previous Pico
firmware retained 200 ms in both modes. Version 0.1.47 changes only the selected
AirPods ACL, after a fresh buffer-target report. Phone and non-flushable control
traffic retain their existing policy. Command acknowledgements are matched by
handle and connection epoch; failure is bounded and visible in `[FLUSH]` status.

The reference is NTPods' `docs/MACOS-AUDIO-CAPTURE-2026-10-12.md`, Working gaming
transition, backed by the saved HCI command and successful completion in
`codec-game-same-process-20261026-control-evidence.json`.

This corrects a captured protocol difference; it is not yet a verified cure for
the reported hitches. The longer scanning-off comparison still produced hitches,
with no active PCM drops, inserted silence, encoder failure, or controller flush
events. Submission counters do not establish timely arrival at the earbuds.

## Checks and remaining validation

Production-handler tests cover the volume cases above, timeout commands and
acknowledgements, failed commands, timer wrap, reconnect/handle reuse, superseded
targets, microphone fallback and post-priming Game Mode verification. Source
clock, packet format, bitrate and float USB alternatives are unchanged.

All 62 C sanitizer checks and 63 Python checks passed, with one optional skip.
SDK volume, phone-codec and LHDC checks, the standard float-capable firmware
build, USB descriptor validation and linked-memory checks passed. The worst
codec allocation leaves 66,200 bytes against the 65,536-byte runtime reserve.

The original PC board was backed up, updated and verified as 0.1.47. Its saved
pairing region matched byte for byte. Startup confirmed the controller's 200-ms
timeout, with no pending or failed command. Playback diagnostics subsequently
confirmed USB float alternative 2 with no active PCM loss or zero insertion;
Windows volume read 78.00%, below the former 85% sticking point.

Listener confirmation, pause/resume, the 60-ms controller transition and a long
Game Mode listening run remain pending. The phone board remains on 0.1.46.
See the [additional capture audit](PROTOCOL-CAPTURE-AUDIT-2026-10-29.md).
