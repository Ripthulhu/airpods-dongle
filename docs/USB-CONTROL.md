# USB controls, version 1

Firmware 0.1.8 retains the version-1 reports and adds the typed features described
in [control extensions](CONTROL-EXTENSIONS.md).

Audio and the microphone use the Windows USB Audio 2 driver. Media keys use HID.
NTPods uses a separate vendor HID collection on the same interface; opening it
does not connect Bluetooth, start audio, or enable the microphone. CDC remains
a debug console. No custom Windows kernel driver or Test Mode is required for USB.

Identify the collection by usage page `FF31`, usage `0001`, feature report length
64 and the `APDG` version handshake. The development USB VID/PID is `CAFE:4015`;
it is not sufficient identification by itself. Never open a consumer-keyboard
collection for commands. This version requires exactly one dongle; commands are
disabled when multiple dongles are present.

All offsets include HID report ID 2 at byte 0. Integer fields are little-endian.
Both directions have bytes 1–4 `APDG`, byte 5 version 1 and byte 6 length 64.
Unknown versions, malformed lengths and nonzero reserved command bytes are
rejected. GetFeature reads a cached snapshot; SetFeature queues at most one
command. Bluetooth work runs on core 0, outside the USB interrupt.

## Status report

| Offset | Meaning |
|---|---|
| 7 | Flags: A2DP connected (1), AACP ready (2), streaming (4), phone connected (8), microphone active (16), phone pairing (32), USB playback (64) |
| 8–11 | Connection generation; changes on selected pair, AACP session or USB session change |
| 12–17 | Selected AirPods address, normal display order; zero if none |
| 18 | Reported listening mode: 0 unknown, 1 off, 2 ANC, 3 transparency, 4 adaptive |
| 19–21 | Left, right, case battery percent; 255 unknown |
| 22 | Charging flags: left 1, right 2, case 4 |
| 23 | Output codec: 0 unknown, 1 SBC, 2 AAC, 3 AAC-ELD |
| 24–35 | Three independent battery ages in milliseconds; FFFFFFFF unknown |
| 36–39 | Last command sequence |
| 40 | Result: 0 none, 1 accepted, 2 busy, 3 stale target, 4 unsupported, 5 disconnected, 6 malformed |
| 41 | Last command opcode; 0 for an invalid opcode |
| 42–43 | Capabilities: pair/reconnect (1), confirmed noise-control support (2), phone pairing (4), settings transport (8), typed feature RPC (16) |
| 44–63 | Reserved zero |

Acceptance means queued/performed locally, not device readback. In particular,
ANC remains its last reported value until the AirPods report a new value.
Battery slots omitted from a report retain their independent age. Session changes
clear all reports. No raw addresses or HID paths need to appear in the UI.

## Command report

Byte 7 is opcode: 1 reconnect selected AirPods, 2 start AirPods discovery/pairing,
3 change listening mode, 4 open the 120-second phone pairing window.
Bytes 8–17 must match the fresh status generation and selected address.
Bytes 18–21 hold a nonzero increasing command sequence (wrap skips zero), byte 22 the argument, and 23–63
are zero. Only opcode 3 uses an argument: 2 ANC or 3 transparency. Other modes
are not exposed by this report.

Only one command may be outstanding. The host waits for the matching sequence
and opcode on the same USB session, with a two-second deadline, and does not retry a timed-out
command. Repeated sequences cannot execute twice. USB removal/suspend clears
queued commands. Pairing remains explicit; status polling never acquires audio.
Reconnect and pairing may change the generation before the receipt is read;
receipts survive those changes, while commands still require a fresh target.
USB reset starts a new sequence session. Old/repeated sequences are ignored.

## Settings transport

Feature report ID 3 in the same vendor collection carries Hearing Aid, Media
Assist, complete hearing-profile edits, saved hardware checks, read-only hearing
metadata and guarded in-case self-checks. Typed feature requests also provide
device settings, output profiles, guarded Game Mode, performance readings and a
selected-pair nearby finder. [Control extensions](CONTROL-EXTENSIONS.md) defines
their payloads. Dongle state remains separate from the PC Bluetooth connection.
Sensor streaming, case ringing and companion handoff are not exposed.

| Offset | Meaning |
|---|---|
| 0–6 | Report ID 3, `APDG`, version 1, length 64 |
| 7 | Operation: ATT (1), AACP (2), cached report (3), select response page (4), cancel (5), commit (6), typed feature (7) |
| 8–11 | Connection generation from report 2 |
| 12–15 | Nonzero increasing transaction sequence; wrap skips zero |
| 16–17 | Total payload length, at most 512 bytes |
| 18–19 | Fragment/page offset |
| 20 | Payload length in this report, at most 42 bytes |
| 21 | Request: zero. Response phase: idle (0), assembling (1), running (2), complete (3), failed (4) |
| 22–63 | Payload, followed by zero padding |

Operations 1–3 and 7 upload contiguous fragments with the same operation, target,
sequence and total length. Assembly responses report the received byte count
as their offset, with no payload. Identical repeated fragments are harmless;
changed overlaps and gaps fail. Nothing reaches Bluetooth until a separate
commit with zero total, offset and payload. Repeated commits cannot replay writes.

A completed response is immutable until the next transaction. Read its pages
using operation 4 with the same sequence and generation, the desired offset,
and zero total/payload length. Response operation remains the original operation.
Failure responses contain one byte: malformed (1), busy (2), stale (3),
disconnected (4), timeout (5), transport failure (6), unsupported (7) or cancelled (8).
Malformed new requests may be ignored; hosts must bound their waits.

Cancel uses the same sequence and generation with zero total, offset and payload.
It discards pending work and closes ATT; it cannot undo a write already sent or
stop a physical self-check. USB loss or a generation change clears the transaction.
USB and the bonded phone have separate assembly buffers, sequences, receipts and
completed responses. Shared ATT/AACP execution has one owner; a competing commit
returns busy while it runs. One client cannot page or cancel another's response.
Assembly and execution have a 3.5-second firmware watchdog. ATT has a three-second
deadline and closes its channel on failure so a late response cannot satisfy a
later request. NTPods never retries a write after an uncertain result.

Cached report requests contain one slot byte: model/firmware (1), Hearing Aid
`09/2C` (2), assistance gate `09/33` (3), PME `53` subtype 2 (4), ear state `06` (5).
For slots 1–5, a present response is its age in milliseconds (`u32`), report revision (`u32`)
and original AACP packet; an empty response means unknown. The revision advances
on every matching report, including an identical or malformed replacement, and
resets with the connection session. Oversized replacements clear the slot;
bounded malformed packets reach the host parser and clear its known state.
Microphone and sensor payloads are never copied into these reports.
Slot 6 returns the versioned settings snapshot defined in
[control extensions](CONTROL-EXTENSIONS.md), rather than a raw packet.
Ear detection uses fresh slot-5 reports and player ownership in the host app;
the firmware never sends a global resume command for ear reinsertion.

ATT opens Basic-mode PSM `001F` on the selected, encrypted Classic link; it
does not create another radio connection. Firmware bounds packet types and
sizes; NTPods validates characteristic discovery, known layouts and settings.
The host binds requests to the exact USB session and connection generation.
Reconnect or unplug invalidates queued commands. Competing pairing/control
commands cannot interrupt a settings transaction. Microphone use blocks raw
ATT/AACP requests and typed device-setting writes; cached diagnostics remain
readable, and finder scanning pauses until capture ends.

Profile edits preserve the complete original record and verify device readback.
Mode changes require AACP report revisions newer than the pre-write baseline,
not USB acceptance or host timing estimates. If the firmware does not report a
changed mode after a notification request, NTPods returns an
unconfirmed result and requires a reconnect/read; it does not reopen AACP
automatically. This behavior needs hardware verification.

Reading saved checks or hearing metadata never runs a test or changes the clock.
Starting a self-check requires explicit confirmation, fresh reports that both
buds are in the case, idle playback/recording, no phone connection, and the known
Pro 3 model/firmware combination. Firmware rechecks fresh in-case reports and
idle audio at the actual send, including retries. This extra guard covers the
single-byte ATT Write Request (`12`, value `31`) used by the self-check command.
Cancellation stops waiting only. Model-specific guards match NTPods; raw
calibration data is not a calibrated hearing test.

USB audio and microphone remain available without NTPods. Hardware validation
is pending the Pico 2 W.
