# Bounded Game Mode timeout comparison — 0.1.49

The 0.1.48 four-minute Game Mode run produced a reported audible hitch and 25
controller Flush Occurred events. Active PCM drops, inserted silence and timeline
recoveries stayed at zero. These events identify discarded data, not an exact
lost-packet count, and the packet-age counter ends at HCI submission rather than
radio delivery. The native Mac trace also marks media automatically flushable.
Another chat was building Android software on the PC during the earlier run;
that possible host-load difference must be excluded from the next comparison.

Version 0.1.49 adds diagnostic feature 18, documented in
[CONTROL-EXTENSIONS.md](CONTROL-EXTENSIONS.md). It temporarily requests a 200 ms
controller timeout while preserving Game Mode's receiver target, packet grouping
and clock policy. It does not change the default timeout policy or volume path.
The override expires after 120 seconds, cannot be renewed by repeated enable
requests, and clears when the relevant connection, ownership or eligibility ends.
Acceptance of the request is distinct from the controller's acknowledgement.

The planned comparison keeps the same music, USB float input, volume and Game
Mode state: confirmed 60 ms, confirmed 200 ms, then confirmed 60 ms again. Sparse
snapshots and listener reports assess each interval. A new connection, route or
manual Game Mode change invalidates the comparison. Cleanup must not overwrite
a user's later mode choice or send commands to a replacement target. No packet
logging or audio-payload inspection is required.

## Checks and installed state

The full suite passed 62 C sanitizer checks and 64 Python tests (one optional
skip), SDK volume checks, phone-codec checks and LHDC checks. The standard
float-capable firmware build, descriptor validation and memory check passed.
Worst-case codec allocation leaves 66,200 bytes against a 65,536-byte reserve.
Tests cover expiry across timer wrap, nonrenewal, cancellation with a pending
acknowledgement, disconnect and eligibility changes, malformed commands and
older-firmware rejection. Independent review found no blocking issue.

The first full build caught a missing test-fixture stub; the corrected build
also includes the collector's explicit feature-18 allowlist and transport test.
Frozen source archive:
`808e20c01f10a605845b6f24cd9c94ba8e617e81c130f574e5cd8ceecf6c3090`.
Verified UF2:
`566d1598eab57dc40a69ac30c816a63ab6d826a8ba6c1a8dda5d3427bd1adda3`.

The original PC Pico was backed up, flashed and verified as 0.1.49. Its pairing
storage matched byte for byte. Startup confirmed the normal 200 ms timeout with
no pending or failed command. The phone Pico remains on 0.1.46. The controlled
listening comparison is pending; this diagnostic is not a verified hitch fix.
