# USB controls, version 1

Version-1 reports include the typed features described in
[control extensions](CONTROL-EXTENSIONS.md).

Audio and the microphone use the Windows USB Audio 2 driver. Media keys use HID.
NTPods uses a separate vendor HID collection on the same interface; opening it
does not connect Bluetooth, start audio, or enable the microphone. CDC remains
a debug console. No custom Windows kernel driver or Test Mode is required for USB.

## Stem volume

Candidate 0.1.38 uses a fresh, transaction-matched native AVRCP INTERIM readback after a
CHANGED notification. The CHANGED payload itself no longer becomes a USB key.
The pinned SDK patch exposes subsequent volume INTERIM replies and keeps their
registration generations separate. The extra post-SET registration introduced in
0.1.37 is removed: it did not complete in the failed Pixel check, while ordinary
native notifications continued. A SET acknowledgement updates the known physical
level; it does not prove equal gain in both buds.

The previous 150 ms quiet period before hardware-gain repair is restored as a
mitigation for the repeatable 0.1.37 balance failure. Verified changes and completed
USB keys restart that period; a genuine host SET remains immediate. This is not
an accepted seamless-volume fix: 0.1.36 already had an audible second step.
A fresh absolute readback can coalesce rapid changes, so
it also cannot establish an exact count of physical gestures. See the saved
[Mac and Pixel evidence](VOLUME-EVIDENCE.md) before changing volume roles or
adding guessed PCM gain compensation.

The underlying relay introduced in 0.1.36 forwards unsolicited AirPods volume changes as standard Consumer
Volume Increment (`0x0C:0xE9`) or Volume Decrement (`0x0C:0xEA`) press/release pairs.
USB playback or recording must own audio and physical AirPods gain must be known.
A genuine host USB Audio volume SET remains authoritative. Android may send that
SET only during initialization, then change software volume for all user actions;
absence of another SET no longer disables the relay.

The temporary AirPods change is withheld from USB volume notifications while
waiting for the host, preventing a hardware-volume notification and HID key from
applying the same gesture twice. After all queued volume keys complete their USB
transfers, the relay restores its confirmed hardware gain anchor if no newer host
SET superseded it. This avoids retaining both the phone's software attenuation
and the AirPods' gesture attenuation. Initial AVRCP readback, unchanged notifications
and feedback matching an outstanding volume command do not generate keys.
The bounded queue keeps rapid gestures in order; USB reset,
route or peer changes invalidate unsent volume keys. A sent press still receives
its release. Expired or rejected keys retain the actual physical gain instead of
restoring it. Reset, route loss and peer changes cancel pending restoration.
Losing AACP audio ownership also invalidates the physical baseline. Only a new,
genuine host SET received while standing by is deferred for the next confirmed
takeover. Otherwise, a fresh native readback must establish the new baseline;
the dongle does not replay the old owner's gain.

This is independent of both companion apps and leaves the existing Bluetooth
phone-volume path unchanged. A known maximum anchor can move one step quieter
to leave observable upward-gesture headroom; zero or unknown gain is never
automatically raised. A clamped or rejected headroom adjustment is not retried
indefinitely. A completed USB HID transfer does not prove the host applied the
key. Endpoint behavior, rapid gestures and ignored host keys therefore require
real-host checks before claiming full slider-range synchronization.

The 0.1.35 Pixel evidence showed two HID keys, 29 physical change reports and
AirPods gain falling from 127 to 47, while the host SET revision stayed at 2.
The old five-second timeout then disabled forwarding. This confirms double
attenuation and an invalid hardware-feedback assumption in 0.1.34; moving the
phone slider was not a successful fix. The 0.1.36 ownership correction also
failed listening: the slider followed gestures, but levels could still diverge,
and a down-swipe audibly dipped before the delayed hardware repair raised it
slightly. Its deliberate 150 ms delay is not an accepted synchronization result.
The counters stayed unchanged during a later quiet check, so a sustained feedback
loop was not demonstrated on that hardware. Synthetic post-ACK stale/quantized
notifications exposed further robustness gaps; do not label those sequences as
captured AirPods behavior.

Target-role absolute-volume requests are validated and counted separately from
controller-role physical-gain notifications. They do not overwrite physical
readback or independently emit a second key for a possibly identical gesture.
Marked target-event/endpoint evidence is required before enabling another relay.

The usage codes follow the [Android USB headset specification](https://source.android.com/docs/core/interaction/accessories/headset/usb-headset-spec).
Android's [default mixer behavior](https://source.android.com/docs/core/audio/preferred-mixer-attr)
can also apply software volume; a USB AudioControl interrupt alone does not
establish that the phone's media-volume UI follows hardware changes.

## USB bootloader entry

Open the identified board's USB CDC serial port with DTR asserted and send the
exact lowercase line `bootsel` with LF or CRLF. From 0.1.46, capability 256
indicates that this explicit maintenance request may interrupt playback, capture,
calls and pending audio transitions. It does not wait for the host to release
its audio interfaces. Ordinary discovery, status requests and physical button
gestures do not authorize an update.

`[BOOTSEL] ready` means the request was accepted, not that boot ROM is already
running. Firmware allows 20–100 ms for the reply to drain, verifies that the same
DTR session is still connected, then calls the Pico SDK's `reset_usb_boot(0, 0)`
from main context outside the Bluetooth lock. On RP2350 this resets the whole
chip into ROM, stopping USB audio and both cores. No asynchronous audio cleanup
or endpoint-idle wait can prevent the explicitly requested update. Verify that the same board
serial reappears as `2E8A:000F` before using picotool. Firmware installation is a
separate action; entering BOOTSEL does not erase flash or pairing storage.
Versions 0.1.34–0.1.45 reject active audio or transitions with `[BOOTSEL] busy`.
Those versions may need one physical BOOTSEL entry to install 0.1.46. Do not loop
or assume an old board supports interruptible entry without capability 256.
The existing bare `t` diagnostics command remains available.
Version 0.1.35 adds two manual `[VOLUME]` status lines to `t`: actual host SET
revisions/session, HID receipts, relay state, physical readback,
pending AVRCP commands and valid volume-event counts. These counters do not
change gain or print per packet. From 0.1.36, `sent` counts completed USB volume
press transfers, rather than reports merely queued at the endpoint. The relay
also exposes its hardware anchor, pending restoration and target-role SET count.

From 0.1.37, bare lowercase `v` resets and arms a volume-only history for 60
seconds; wait for `[VOLTRACE] armed` before the test gesture. Uppercase `V`
freezes and dumps at most 16 chronological records. Keep DTR asserted throughout:
closing the console cancels recording and dumping. Ordinary `t` does not arm it.
The ring uses 256 bytes for records, never allocates and never prints from a
Bluetooth callback. Main emits one short line at a time after earlier CDC output
drains. A later arm clears the previous history.

Each `[VOLTRACE]` row contains four hexadecimal 32-bit words: wrapping microsecond
time; event/ctype/transaction-label/state/status metadata; incoming, previous,
desired and sent level bytes; and low 16-bit completed/expected HID counters.
The versioned header and `volume_trace.h` define the layout. This captures control
ordering only, not audio, microphone payloads or full Bluetooth traffic.

## Companion collection

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
| 24–35 | Three independent battery ages in milliseconds; known ages saturate at FFFFFFFE, FFFFFFFF unknown |
| 36–39 | Last command sequence |
| 40 | Result: 0 none, 1 accepted, 2 busy, 3 stale target, 4 unsupported, 5 disconnected, 6 malformed |
| 41 | Last command opcode; 0 for an invalid opcode |
| 42–43 | Capabilities: pair/reconnect (1), confirmed noise-control support (2), phone pairing (4), settings transport (8), typed feature RPC (16), settings workflow lease (32), battery observation flags (64), USB host volume authority (128), active-audio runtime update (256) |
| 44 | With capability 64: component present mask, left 1, right 2, case 4 |
| 45 | With capability 64: components observed through case BLE association, same bit order |
| 46–63 | Reserved zero |

Acceptance means queued/performed locally, not device readback. In particular,
ANC remains its last reported value until the AirPods report a new value.
Battery slots omitted from a report retain their independent age. Session changes
clear connected readback. Selected-pair case observations can survive same-peer
AACP loss; changing pair or key clears them. No raw addresses or HID paths need
to appear in the UI.

Firmware 0.1.11 adds capability 64. Updated apps must use its presence mask:
an absent component may retain a percentage and its original age, with presence
and charging cleared. Older firmware keeps the original interpretation. Case
provenance means keyed layout association, not IRK-resolved or authenticated
identity. It must never authorize audio, pairing, ear actions or finder results.
See [case BLE observations](CONTROL-EXTENSIONS.md#case-ble-battery-observations).

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
It discards pending work, including an unsent typed setting write, and closes ATT;
it cannot undo a write already sent or stop a physical self-check. A valid workflow
lease remains available for mode-restoration cleanup; the app releases it when
the whole action ends. USB loss or a generation change clears both.
USB and the bonded phone have separate assembly buffers, sequences, receipts and
completed responses. Shared ATT/AACP execution has one owner; a competing commit
returns busy while it runs. One client cannot page or cancel another's response.
Assembly and execution have a 3.5-second firmware watchdog. ATT, raw AACP and
typed setting sends have three-second deadlines. Typed writes remain running
until sent locally, then require separate device readback. Client/session ownership,
microphone restrictions and deadlines are rechecked before every radio send,
including retries. ATT closes its channel on failure so a late response cannot
satisfy a later request. NTPods never retries a write after an uncertain result.

Raw ATT/AACP transactions require a settings workflow lease. Acquire it before
reading an edit's baseline and retain it through writes, readback and mode cleanup.
Capability 32 and [typed subcommand 10](CONTROL-EXTENSIONS.md#settings-workflow-lease)
define the handshake. Cached reports remain readable without a lease. Apps must
fail closed if firmware lacks this capability; an unlocked edit is not a fallback.

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

USB audio, microphone and stem controls have worked on Pico 2 W without NTPods.
The current candidate's remaining hardware checks are recorded in
[audio continuity](AUDIO-CONTINUITY.md); prior listening results do not validate
every later firmware change.
