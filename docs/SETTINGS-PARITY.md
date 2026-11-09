# Settings parity: firmware and CLI

Reviewed against LibrePods core and its guarded dongle controller on 2026-11-09.
Firmware 0.1.64 keeps the existing HID protocol, radio .0076, audio formats,
volume synchronization and connection policy. All commands are documented by
`python tools/airpods.py help`; these changes do not add Android UI controls.

Existing feature-2 and cached-slot-6 snapshots keep their original 23 IDs for
installed NTPods versions that reject unfamiliar IDs. The CLI opts into EN 352
and Gain Swipe readback through feature-2 `[2,1]`; no Windows app update is needed
to keep its existing settings working.

Firmware 0.1.65 adds the music behavior behind two existing settings: Conversation
Awareness speech events drive smooth, temporary PCM attenuation on USB and relay
music; Automatic Ear Detection drives USB Consumer Pause/Play on wear transitions.
Neither needs a companion app or custom Windows/Linux driver. These actions
respect current settings and ownership, never change host/hardware volume, and
stop during known dongle call/microphone paths. Conversation Awareness uses a
uniform quarter-amplitude reduction for active speech stages, rather than
Android's configurable stage behavior. Dedicated Play/Pause support depends on
the host/player; generic USB cannot track every manual media action or identify
a host call using the host's own microphone. Physical Windows/Linux and listening
checks remain outstanding; see the
[release notes](releases/0.1.65.md).

| Setting | Previously missing | Path and validation |
| --- | --- | --- |
| EN 352 Protection | Firmware catalog and CLI | AACP `09/37`, 1 on/2 off; known Pro 3 only; received valid scalar baseline |
| Gain Swipe | Firmware catalog and CLI | AACP `09/2F`, 1 on/2 off; known Pro 2/3; received valid scalar baseline |
| Smart Routing / Allow Auto Connect | CLI names | Existing `09/20`, `09/36`, ordinary toggle encoding and model guards |
| Game Mode | Normal CLI command | Existing typed features 4/9, reports requested vs confirmed/waiting separately |
| Loud Sound Reduction | CLI transaction | ATT UUID `a48fec08-3921-43db-82aa-afbce8ebb4fb`; strict byte 0/1; discovered advertised write properties, exact readback |
| EQ | Firmware raw allowlist, cache and CLI | Strict opcode 63 packet; new cache slot 7; current model/report required and queued-revision fence |
| Hearing Aid / Media Assist | CLI transaction | Existing raw AACP plus cache slots 2–4; enrollment, fitting, mutual exclusion and fresh mode confirmation |
| Headphone Accommodations / Audiogram | CLI transaction | Existing 140-byte PME subtype 2 plus coupled Custom Transparency; preserve fitting, enrollment and unrelated adjustments |
| Hearing / Custom Transparency adjustments | CLI transaction | Existing discovered ATT characteristics, full-record compare/patch/readback; no fabricated default record |

## Hearing transaction details

Hearing Aid uses the received 11-byte `09/2C` record, preserving byte 7 enrollment
and changing byte 8 active state with 1/2. The `09/33` gate is shared: it is the OR
of Hearing Aid and the PME Phone/Media processing flags. It is not an independent
Hearing Aid switch. Enabling Hearing Aid also selects Transparency (`09/0D=3`).
Media Assist changes PME bytes 10/11, preserving the enrollment byte and all 128
curve bytes. On requires an enrolled, writable fitting and Custom Transparency
off. Off retains the guarded recovery rules without requiring a new fitting.

Legacy Headphone Accommodations requires clinical Hearing Aid and its shared gate
off, with PME enrollment 0 or 2. Sending PME sets byte 9 to 2, separately from
the expected received enrollment value. The nine stock curve blocks match
LibrePods' anonymous macOS captures. Audiogram normalization uses the validated
stored fitting; it never generates or installs a hearing test result. Preset
edits send PME then its coupled Custom Transparency curves and verify the fitting
and clinical guards afterward. Phone/Media switches preserve the existing curves.

Fitting UUID is `a4120005-95c5-4d6f-9098-0f0b41457e0a`; supported records are
104 bytes with prefix `02 {00|02} {60|64} 00`. Only enrollment 02 is writable.
Writers use `02 02 64 00`; readback can retain 60, so confirmation validates the
header and compares the 100-byte payload rather than inventing a prefix mismatch.
Curve and adjustment ranges mirror the native validators.

Custom Transparency UUID is `d5621cc1-f7ab-45db-9403-9eaf744d5390`. Valid 100/104-byte
little-endian float records retain all unedited bytes, including signed zero.
Own Voice requires the 104-byte form; coupled preset editing requires the reviewed
104-byte form. The separate four-byte inactive sentinel can establish Off while
enabling a clinical mode, but cannot serve as an editable tuning baseline.

All complex reads/edits acquire feature-10's workflow lease, fence generation and
selected peer, and discover characteristics by UUID. A lease never survives a
target change or gets reacquired midway through an edit. Readback polling is
bounded; writes are sent once. Partial application is reported as uncertain,
without replaying writes or overwriting a newer external choice.

## Deliberately unavailable

- PPE cap-level `09/38`: named in native code, but no verified range or setter.
- Head gestures: Android uses local preferences and motion detection; `09/39`
  delegates raw stem controls and is not a head-gesture enable switch.
- Hearing threshold editing, protected calibration access, test enrollment and
  diagnostic self-checks: no new CLI commands. These are separate explicit workflows.

Model and received-record gates remain authoritative. Exposing a command does
not establish support on every AirPods model, firmware or host audio path.
