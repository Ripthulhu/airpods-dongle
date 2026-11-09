# Dongle control extensions

The [version-1 USB reports](USB-CONTROL.md) provide phone
controls, device settings, output profiles, Game Mode, performance readings and
nearby finding. The implementation and host tests are complete; Pico/AirPods
hardware verification is pending. The default reconnect policy is Manual;
ordinary polling never requests ownership unless Automatic was explicitly saved
with feature 23.

All integer fields are little-endian. Layout lengths below are exact.

## Phone transport

An encrypted RFCOMM service on the saved, bonded phone connection exposes the
same reports as USB. UUID: `c6c4f8a0-6d47-4bd1-9b6a-415044470001`.
Each frame has an eight-byte header: `APDG`, version 1, operation, report ID,
payload length. Operation 1 reads (length zero); operation 2 writes (length 64).
Report IDs are 2 (basic controls) and 3 (settings RPC). A response uses operation
`0x81` or `0x82` and contains the full 64-byte report, including its report ID.
An error response uses operation `0xff`, the requested report ID and one error
byte: busy 2 or unavailable 3; code 1 is reserved for malformed requests.
Hosts keep only one request outstanding.
RFCOMM is a byte stream: accept fragmented headers/payloads and coalesced frames.
Unknown framing closes the channel. Four request and response slots bound
queued work, and 3.5-second deadlines close partial frames or blocked writers.
The service checks the saved peer, encryption and link key before accepting a
connection and before processing or sending each request/response.
Identity keys are never included in the control responses.

USB and phone have separate command sequences, assembly buffers, receipts and
completed responses. Shared ATT/AACP execution has one owner; competing commits
receive busy while it runs. One client cannot read, cancel or overwrite another's
response. Losing that client cancels its pending operation. Merely connecting
or polling never starts audio.

## Settings RPC additions

Report 3 retains its version-1 layout. Upload operation 7 is a typed local feature
request, staged and committed exactly like ATT/AACP requests. Its first byte is:

| Subcommand | Request after the subcommand byte | Successful response |
| --- | --- | --- |
| 1: device setting | control ID u8, value u32 little-endian | empty after local transmission; device readback is separate |
| 2: settings snapshot | none (legacy), or selector u8=1 (extended) | cached setting records below |
| 3: audio profile | output codec u8 (0 auto, 1 SBC, 2 AAC, 3 ELD), rate mode u8 (0 CBR, 1 VBR), bitrate u32 in bits/s | profile status below |
| 4: Game Mode | enabled u8 (0/1) | 20-byte profile status; device confirmation is separate |
| 5: performance | none | versioned diagnostic snapshot |
| 6: start nearby finder | none; identity comes from the selected pair's AACP key response | finder snapshot |
| 7: nearby status | none | finder snapshot |
| 8: stop nearby finder | none | empty |
| 9: audio profile status | none | profile status below |
| 10: settings workflow lease | action u8, token u32 | acquire: version u8=1, token u32; renew/release: empty |
| 11: audio trace | none | read-only 512-byte version-1 snapshot; see audio continuity document |
| 12: diagnostic BLE scan policy | enabled u8 (0/1) | empty; disabling expires after 120 seconds |
| 15: phone input counters | none | read-only queue/decoder snapshot: v1 52 bytes, v2 64 bytes |
| 16: physical volume snapshot | none | 24-byte version-1 status; capability 128 |
| 17: USB host volume authority | action u8, token u32, level u8 | 24-byte status; USB client only; capability 128 |
| 18: diagnostic Game Mode flush comparison | enabled u8 (0/1) | empty; USB client only; see below |
| 19: radio and adaptive bitrate | none | read-only, cached 128-byte version-1 snapshot; USB or phone |
| 20: routing status | none | 32-byte version-1 snapshot; capability 512 |
| 21: authorized handover | action u8 (0 release, 1 acquire), expected intent revision u32, nonzero request nonce u32 | routing snapshot; capability 512 |
| 23: reconnect policy | query: none; set: mode u8 (0 Manual, 1 Automatic) | version u8=1, effective mode u8 |

## Explicit routing handover

Firmware 0.1.59 adds capability 512. The basic Connect command now requests the
same bounded takeover as a physical button press. Source release uses feature 21,
never the destructive Pair command. Both routing operations retain the selected
pair and its saved keys. Release refuses microphone/call use, codec transitions,
media recovery and pending Game Mode writes/restoration. A confirmed active Game
Mode first restores normal buffering and the local clock on that same owned
connection before release; its requested preference is retained. A release disables incoming
media until a later explicit acquisition or button request. This hold is volatile;
a firmware restart requires a new host session and fresh policy checks.

Feature 21 requires the current intent revision from feature 20. Acceptance
advances that revision and records the request nonce and action. Repeating the
old revision or reusing the current receipt's nonce is stale; hosts must not
retry the operation. Physical button, basic Connect and Pair choices advance the
revision and clear the automatic receipt, including coalesced takeover presses.
Source playback and routing policy remain host responsibilities by default:
audio packets, proximity and notification sounds do not authorize acquisition.
Feature 23 provides a separate, explicit opt-in to autonomous saved-peer retries.

| Snapshot offset | Field |
| --- | --- |
| 0–1 | Version 1, length 32 |
| 2 | Flags: ownership known 1, owned 2, takeover pending 4, released 8, release pending 16, A2DP connected 32, AACP ready 64, microphone/call busy 128 |
| 3 | Takeover stage |
| 4–7 | Nonzero intent revision |
| 8–11 | AACP control session |
| 12–17 | Selected AirPods Classic address |
| 18 | Last automatic action: none/manual 0, release 1, acquire 2 |
| 19 | Reserved zero |
| 20–23 | Local monotonic uptime in milliseconds |
| 24–27 | Pending takeover age in milliseconds, otherwise zero |
| 28–31 | Last accepted automatic request nonce, otherwise zero |

An accepted release is not its completion. Released requires actual signaling
closure, no pending media connection and no local audio authority. While closure
is pending, automatic acquisition returns busy; a newer manual choice can still
supersede it. Acquisition requires positive remote ownership evidence through
the existing routing exchange before the host reports success. AACP session and
report-2 generation may change during release. Hosts recover its receipt using
fresh feature 20 reads tied to the same USB session, selected pair, new intent
revision and exact echoed nonce/action. A missing receipt never authorizes a
retry or source reconnection. Settings workflow leases block routing writes,
including writes from their owner; read-only routing status remains available.

## Reconnect preference

Firmware 0.1.63 adds feature 23 with exact payloads `[23]` to query and `[23, mode]` to save, with
`mode=0` for Manual or `mode=1` for Automatic. Both return exactly `[1, mode]`
on success. No report-2 capability bit or existing report layout changes.
Older firmware returns unsupported (RPC error 7) for feature 23. Feature 22
remains reserved for the existing USB output-continuity contract.

Manual is the default, including after upgrades with no saved preference. A
button press, basic Connect or feature-21 acquisition still grants the existing
bounded takeover intent; a saved AirPods pair may also initiate an incoming
connection. Automatic authorizes outgoing retries only to the selected saved
address with a valid bond, using the existing 1/2/4/8-second backoff. It never
starts discovery. **Automatic may interrupt another host using that pair**;
Manual is recommended when the AirPods are shared between devices.

A successful policy write takes effect on subsequent eligible retries, which
may happen immediately. Explicit release suppresses incoming connections and automatic retries
until Connect, a button request or reboot. Changing the preference cannot clear
that hold. Saving Manual cancels an outstanding automatic takeover intent but
preserves an explicit user request. An already-started connection may finish;
new policy writes return busy during physical connection or audio transitions.

The preference occupies the separate `APRC` TLV tag (`0x41505243`): exactly two
bytes, version 1 and mode 0/1. Pair-slot `APSL` and bond formats are unchanged.
Missing `APRC` means Manual and does not require a flash write. Wrong lengths,
unknown versions, invalid modes or unavailable storage fail closed to Manual
and return unsupported without overwriting the record. Writes are acknowledged
only after exact schema and value readback; unchanged preferences avoid writes.
The verified runtime preference is loaded once at startup, with no repeated
preference reads in the audio polling loop.

Policy writes return busy (RPC error 2) during playback, microphone/call use,
codec/media recovery, connection or Game Mode transitions, and while any settings
workflow lease exists, including the caller's own lease. Read-only queries remain
available during playback and leases. Malformed requests return error 1.
If a store or post-write verification fails, RPC error 6 means the change may
have persisted: **do not retry automatically**. Runtime retries fail closed to
Manual, and subsequent queries return unsupported rather than claiming the old
value or reactivating uncertain stored Automatic. A later explicit verified set
or restart can reload a valid record; future unknown formats remain protected.

The explicit [host volume authority contract](HOST-VOLUME.md) coordinates a host
that actually delegates its mixer volume to the AirPods. It suppresses standalone
UAC gain requests and feedback while leased; simply moving the phone's slider is
not proof of delegated gain. From 0.1.48 the legacy HID/anchor relay is removed
entirely. The host extension uses a separate five-second lease and does not hold
the ATT settings workflow lease.

From 0.1.49, diagnostic feature 18 temporarily substitutes a 200 ms controller
automatic flush timeout while leaving Game Mode's 60 ms receiver target,
two-access-unit packets and clock adjustment unchanged. Send `12 01` to start
the comparison and `12 00` to clear it (hexadecimal bytes). Enabling requires
the selected AirPods ACL's normal Game Mode timeout already confirmed at 60 ms;
otherwise it returns busy. A phone client returns unsupported. Success means
the local policy was accepted, not that the controller acknowledged the change.
Use the existing HCI `FLUSH` desired/confirmed values to verify each phase.

From 0.1.62 ordinary Game Mode retains the normal 200 ms controller timeout,
matching Android's baseline-preserving policy. Feature 18 keeps its original
meaning and 60 ms admission check, so it normally returns busy because the
baseline already matches. It is not reinterpreted as a request for 60 ms.
Cancellation, expiry and late acknowledgements restore 200 ms.

The override expires after 120 seconds, including across timer wrap. Repeated
enable requests and playback polling do not renew it. Disable, selected-peer or
ACL changes, source/control-session or audio-ownership loss, microphone use,
leaving Game Mode, USB-client disconnect and controller reset clear it. Expiry
or cancellation restores the ordinary policy through the same bounded,
per-handle ACK/epoch command engine; a failed or missing ACK remains visible.
It is volatile and has no product UI or persistent setting. The report-3 layout
and capability 16 remain unchanged; older firmware returns unsupported.

Cached report slot 6 also returns the legacy settings snapshot. Format: version u8=1,
count u8, then count records of control ID u8, age milliseconds u32, report
revision u32, value u32. Only device reports create records. Requested values
must remain separate until a fresh matching device report is received. Unknown
IDs and unsupported model/value combinations fail without sending a packet.
Identical reports still advance their revision. Malformed replacements clear
known values instead of preserving a misleading current reading. Reconnects
clear settings and revisions; hosts must fence confirmations to the same session.

Control IDs here are hexadecimal. The reviewed catalog covers microphone
preference (`01`), device ear detection (`0A`), listening modes (`0D`), paired
stem actions (`16`), press speed and hold duration (`17`, `18`), the hold-mode mask (`1A`), one-bud ANC (`1B`),
tone/swipe/call controls (`1C`, `1F`, `23`–`25`), Personalized Volume (`26`),
Conversation Awareness (`28`), adaptive strength (`2E`), case sounds/volume
(`31`, `40`), Allow Off (`34`), Pause When Asleep (`35`), and the reviewed
`20`, `36`, `3B` settings. Firmware 0.1.64 appends Gain Swipe (`2F`) for known Pro 2/3
and EN 352 Protection (`37`) for known Pro 3, both 1 on/2 off and requiring a valid
current report before writing. Existing catalog indices and model masks retain
their meaning. `38` remains rejected: its value format has not been verified.
Availability follows the reported model, firmware and
control readback; this is not a promise that every model supports every control.
Ear detection reverses the ordinary toggle encoding: `0A` uses 2 on/1 off;
ordinary toggles use 1 on/2 off. Microphone preference is 0 automatic, 1 right,
2 left. Press/hold/swipe durations use 0–2; values are not shifted to match
older UI indexes. Left/right actions are sent together, preserving the other
side. Existing hearing/diagnostic RPCs and their restoration guards are unchanged.

Firmware 0.1.64 also adds raw cached report slot 7 for EQ. Its payload is the
same age/revision envelope as slots 1–5, followed by exactly 13 bytes:
`04 00 04 00 63 00 05 00 01 STATE LOW MID HIGH`. State is 1 off/2 on;
each band is 0–100. Slot 6 remains the scalar snapshot. Unknown/malformed EQ
reports never authorize a write. A raw opcode-63 write requires known model
metadata and a valid current EQ report; its baseline revision is captured at
queue admission and checked again at the radio send. Replacing or invalidating
that report retires the queued edit. Hosts preserve untouched fields and require
a newer exact device report, separately from local send completion.

The default feature-2 request `[2]` and cached slot 6 retain the original
23-control catalog. EN 352 (`37`) and Gain Swipe (`2F`) are returned only by
the explicit extended feature-2 request `[2,1]`. Other lengths/selectors are
malformed. This keeps already-installed clients that reject unfamiliar IDs
working without an app upgrade. The CLI requests the extended form for those
fields and `settings`, falling back to legacy reads only when older firmware
explicitly rejects the read form as malformed or unsupported. A busy or
uncertain transaction is never retried. Write validation is identical for both
catalogs; only the snapshot's fields differ.

The [CLI](CLI.md) exposes these settings and the existing leased hearing/ATT paths.
[Settings parity](SETTINGS-PARITY.md) records their layouts and remaining exclusions.

The native hold-cycle setting `1A` is a shared selection mask: Off `01`, ANC `02`,
Transparency `04`, Adaptive `08`. Any supported combination of at least two modes
is accepted, including Off + ANC + Adaptive (`0B`). This chooses membership, not
a custom cycling order or a separate list for each ear. Adaptive requires a
supported model and firmware major 6 or later; Off also requires Allow Off where
the model exposes that choice. The host does not silently enable Off.

Send typed feature bytes `01 1A 0B 00 00 00` for that example. The exact AACP
packet is `04 00 04 00 09 00 1A 0B 00 00 00`. Both USB and phone use this same
path and cached `1A` readback. It never sends `16` or changes either ear's hold
assignment; only ears assigned noise control use the selected modes. A successful
send remains separate from a fresh matching device report.

## Radio and automatic bitrate snapshot

From 0.1.51, feature 19 reports the selected AirPods ACL and the live AAC-ELD
encoder budget. Reading it never starts a radio query, changes a setting or
enables packet logging. Existing audio-profile status continues reporting the
configured local ceiling, including CBR alignment headroom; use this snapshot for
the current adaptive budget. The local ceiling can be below the wire ceiling.

The exact response is 128 bytes, little-endian:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u8 | version 1 |
| 1 | u8 | length 128 |
| 2 | u8 | flags: live selected ACL 1, sampling active 2, query pending 4 |
| 3 | u8 | valid measurements mask |
| 4 | u32 | snapshot uptime, milliseconds |
| 8 | u32 | selected radio connection epoch |
| 12 | u16 | selected ACL handle; FFFF unknown |
| 14, 15 | u8 each | failed and unavailable measurements masks |
| 16 | i8 | Classic RSSI relative to the controller's receive-power window; not absolute dBm |
| 17 | u8 | controller-defined link quality, 0–255, higher is better |
| 18, 19 | i8 each | current and maximum transmit power, dBm |
| 20 | u8 | actual ACL AFH mode |
| 21 | 10 bytes | AFH channel map, lowest numbered channel in the low bit |
| 31 | u8 | reserved, zero |
| 32 | five u32 | measurement ages, milliseconds; FFFFFFFF unknown |
| 52, 56 | u32 each | selected-ACL flush events and completed packet credits |
| 60 | u32 | reserved, zero |
| 64 | u8 | adaptation block version 1 |
| 65 | u8 | flags: active 1, frozen 2, fault 4, VBR 8 |
| 66 | u8 | reason: off 0, startup 1, clean 2, pressure 3, recovery 4, frozen 5, fault 6 |
| 67 | u8 | reserved, zero |
| 68, 72, 76 | u32 each | current, negotiated/user ceiling and pending target bitrate, bits/s |
| 80, 84 | u32 each | successful budget changes and update errors |
| 88 | u32 | last budget change uptime, milliseconds |
| 92, 96 | u32 each | latest observation window's flush and local transport-pressure events |
| 100 | u32 | observation window duration, milliseconds |
| 104 | u32 | minimum budget, bits/s |
| 108 | u32 | continuous clean observation duration, milliseconds |
| 112 | u32 | reserved, zero |
| 116, 120 | u32 each | cumulative local pressure events and selected-ACL flush events |
| 124 | u32 | radio epoch used by the adaptive controller |

Measurement mask bits and age order are RSSI 1, link quality 2, current power 4,
maximum power 8 and AFH map/mode 16. A zero value without its valid bit is unknown,
not a successful reading. Check age as well as validity. Query failure is not
evidence of a poor signal. Completed packet credits can include discarded data;
they are not receiver delivery acknowledgements or a retransmission count.
Link quality is vendor-defined and has no universal signal-strength threshold.

The supplied Windows/Android collector reads only this cached snapshot every
two seconds, with no companion application required:

```text
python tools/audio_diagnostics.py --radio --serial PICO_SERIAL --seconds 600 --output radio.jsonl
```

Android/Linux requires access to the dongle's hidraw interface (root on the test
Pixel). A disconnected or unsupported controller remains explicitly unknown.
Older firmware returns unsupported rather than a fabricated measurement.

## Settings workflow lease

Report-2 capability `0x20` signals this requirement. Subcommand 10 uses action
0 to acquire with token zero, 1 to renew with the returned nonzero token, and 2
to release that token. Acquisition returns five bytes: version 1 and the token.
The token belongs to one client and connection generation; another client cannot
renew or release it. Acquisition while any lease is held returns busy.

Acquire before reading hearing-mode or profile baselines. Hold the lease for the
complete read/compare/write/verify action, including mode restoration. All raw
ATT/AACP commits require it. Other clients' device-setting, audio-profile and
Game Mode writes return busy while it is held; cached reports and local read-only
snapshots remain available between transport requests. Basic pairing/reconnect
commands return busy until the workflow ends.

The lease expires after five seconds without renewal or an owned raw commit,
and after thirty seconds regardless of activity. Renew during cached mode polling.
Disconnect, USB/session change or target change revokes it and cancels unsent work.
Tokens that expire or lose their session must never be reacquired midway through
an edit: start again with a fresh baseline. Apps reject older firmware without the
capability instead of performing unlocked edits.

Cancelling one RPC removes its unsent packet but preserves a valid workflow lease
for mode cleanup. Release in the action's final cleanup, including cancellation;
if release cannot be confirmed, end the action. Firmware expiry bounds abandoned leases.

## Output profiles

| Requested codec | Rate mode | Allowed bitrate in bits/s |
| --- | --- | --- |
| 0 automatic | 0 | 0 |
| 1 SBC | 0 | 0 |
| 2 AAC | 0 CBR | 0/default, 96000, 128000, 174000, 192000, 220000 |
| 3 AAC-ELD | 0 CBR or 1 VBR | 0/default, 128000, 174000, 192000, 220000, 256000 |

Automatic prefers ELD, then AAC, then SBC. Zero AAC bitrate chooses up to
220000; zero ELD bitrate chooses up to 256000, within the peer's ceiling.
AAC is stereo 48 kHz LC. ELD is stereo 48 kHz with 480-sample frames.
VBR additionally requires the peer's advertised VBR capability and the supported
FDK mode. Its peak is floored to a whole byte per 10 ms frame: a 174000 request
has a 173600 bits/s peak. Requested and actual values remain distinct.

The rate mode describes the local encoder, independently of the Apple vendor
configuration's variable-rate permission. When the peer permits variable rate,
the selected wire ceiling remains capped by both peers while a lower local
CBR target or VBR peak can change within it. CBR reserves one alignment byte per
10 ms frame: a 256000 wire ceiling allows a 255200 local CBR budget; a lower
220000 request stays 220000. Profile status keeps the requested bitrate and local
rate mode unchanged and reports the effective local budget as its actual rate;
feature 19 reports the current adaptive budget and its local ceiling, not the
wire ceiling.
A fixed-only peer retains fixed CBR negotiation with no live bitrate adaptation.
If variable permission cannot fit 128000 CBR plus alignment headroom, selection
falls back to that fixed path. An unsupported actual VBR request is rejected.

A profile change returns busy during USB/phone playback, phone preparation,
microphone use, pending connection/profile work or an unresolved Game Mode
restore. Pause first. Accepted changes close and reconnect the selected AVDTP
session for fresh negotiation; no codec is reset in a running stream. A complete
current-peer capability catalog can reject an unsupported explicit choice
before disconnecting. Partial, unknown or truncated catalogs require fresh
negotiation. Explicit choices never fall back silently, and pending negotiation
has a 15-second deadline.
Starting headset pairing cancels any pending profile reconnect before discovery.

Startup priming stays fixed at 100 ms. USB retains at most 1,440 frames during
startup, targets 960 frames remaining after a normal read, and allows 3,840 frames
during active AAC-ELD playback. Larger codec units extend the active capacity.
Excessive stalls reconcile discarded PCM and scheduling debt in one counted
recovery; healthy playback no longer uses the old unconditional 30 ms trim.
The phone queue is unchanged. See [audio continuity](AUDIO-CONTINUITY.md).
Profiles do not change timestamps, packet grouping, PCM allocation or flush policy. These controls report transport
configuration, not an acoustic quality or latency measurement.

## Game Mode

From 0.1.39, Game Mode starts off at boot. Double-press BOOTSEL or use typed
feature 4 to enable it. The runtime choice survives same-peer reconnects and
resets to normal mode at the next power cycle; a companion is not required.
For a supported, owned output, normal startup also requests a fresh 150 ms
preparation once per control/ownership epoch. This clears a low-buffer setting
left by an earlier host or boot. Admission and receipt are bounded; unsupported
headsets and standby dongles do not receive this write.

The current gate requires a recognized Pro 3, firmware major at least 8,
negotiated AAC-ELD and that endpoint's advertised delay-reporting support.
The AAC-ELD Set Configuration also selects the delay-reporting service category;
advertising support alone does not enable reports. Reports must match both the
active connection ID and local stream endpoint.
Firmware requests the reviewed 60 ms receive buffer with AACP control `0B`
after 500 ms of stereo readiness. Firmware 0.1.16 can enter or leave Game Mode
during USB playback. A fresh report selects a finite interval change: 1.1 when
the delay exceeds the target, or 0.9 when below. The absolute difference is the
drift goal, minus 10 ms of headroom on entry when the difference exceeds 10 ms.
The adjustment begins between complete media packets. USB PCM is resampled at
the matching ratio so fixed-rate ingress does not become a new latency backlog.
Successful media sends measure drift against nominal elapsed frames; reaching
the goal returns pacing to normal. A ten-second deadline fails the adjustment
and requests restoration rather than claiming completion. Pauses, microphone
use, peer/session changes and transport stops cancel the temporary rate.
Gaming ELD packets hold at most two access units, versus three normally.
The phone input still prepares while suspended; its live clock is separate.
Candidate 0.1.39 also verifies a prepared USB target after the 100 ms startup
prime has been sent. The pre-Start delay can prepare the transport, but cannot
describe its post-prime fill. An unmet USB verification uses one fresh request
and the existing successful-send, fresh-report and finite live-adjustment path.
It does not reuse the pre-Start report or poll continuously. Peer/session,
ownership and input changes invalidate old proof. An ordinary live request can
satisfy the verification; completed verification does not repeat on a warm pause.
Resume waits for the preparation's report or bounded failure. Ordinary pauses
retain the target; microphone use or disable restores 150 ms. A same-pair AACP
reconnect retains the restore obligation. Missing confirmations time out rather
than retrying forever. The user's preference can stay on while paused.

Pro 3 / 9A348 testing of the live request produced a fresh 159 ms report and no
clear audible reduction. With 0.1.15 and the stream suspended, disabling then
enabling Game Mode produced fresh reports of 150 ms then 60 ms on the same
connection. A paused player can leave Windows' USB stream running; preparation
in 0.1.15 waited for actual stream suspension. These reports do not measure acoustic delay;
the playback comparison remains separate from the paused protocol check.
The listener then confirmed less delay and clean audio after resuming. Five
playback samples retained the same connection, with zero underflows, send
failures or recoveries and no increase in the cumulative PCM drop counter.
The last delay report remained 60 ms; no new report was received during playback.

Queued, sent and confirmed are separate states. Confirmation requires actual
successful command transmission and a new nonzero AVDTP delay report within
two seconds. An old report cannot confirm a new request. This establishes a
transport response, not measured speaker delay or a guarantee that the buds
adopted exactly the requested buffer. During a live adjustment, status remains
pending (or restoring) until its finite rate change ends. On 27 October 2026,
the 0.1.16 live board check completed on/off/on adjustments without a reconnect
or media suspension. The listener confirmed clean, continuous playback in both
directions and less delay on entry. The immediate reports were 76, 65 and 116 ms;
the finite adjustments completed in 32, 815 and 406 ms. These reports precede the
completed clock adjustment and must not be presented as final acoustic latency.
Separately, the user measured total speaker latency at approximately 60 ms
with Game Mode on using a visual test while listening.
No send failures or recoveries occurred. The three comparison windows added no
underflow frames, but the bounded source queue did discard 1296, 384 and 864
frames respectively; this is not a lossless PCM or long-duration stability claim.
The request is armed before enqueueing because BTstack may send synchronously.
The successful-send callback records the delay-report revision, so a report that
arrived while the command was queued cannot confirm it.

## Audio profile status (20 bytes)

Bytes 0–3: version 1, requested codec, actual codec, rate mode. Codec values are
0 unknown/automatic, 1 SBC, 2 AAC, 3 ELD. Requested and actual bitrates are u32 at
4 and 8. Byte 12 is the Game Mode preference; byte 13 is its state: 0 off,
1 pending, 2 sent/unconfirmed, 3 confirmed by a fresh delay report, 4 failed,
5 unsupported, 6 restoring. Byte 14 is input ownership: 0 idle, 1 USB,
2 phone (music or call). Calls keep the existing value for compatibility;
the microphone flag and explicit CDC call snapshot provide additional context.
Byte 15 flags: profile pending 1, streaming 2, microphone 4, Game Mode supported 8.
Delay is u16 at 16 in tenths of milliseconds (`FFFF` unknown); startup priming
is u16 at 18 in milliseconds, fixed at 100. Delay confirmation is transport
evidence, not an acoustic latency measurement.

## Performance snapshot (64 bytes)

Bytes 0–3: version 1, input ownership, actual codec, flags (streaming 1,
microphone 2, phone connected 4, AACP ready 8). The remaining fields are u32:

| Offset | Value |
| --- | --- |
| 4 | Monotonic uptime milliseconds |
| 8, 12, 16 | PCM frames used, capacity, high-water mark |
| 20, 24 | PCM underflow frames excluding priming, dropped frames |
| 28 | Encoded audio units |
| 32, 36 | Last/maximum encode time in microseconds |
| 40, 44 | Send failures, recovery requests |
| 48, 52, 56, 60 | Linked heap capacity, allocated bytes, remaining bytes, allocator arena |

Counters accumulate from boot and wrap modulo u32. Heap remaining is linked
heap capacity minus allocator-reported allocated bytes; it is not a stack or
fragmentation measurement. `FFFFFFFF` means unavailable where defined.
Snapshot reads never run a hardware self-check or acquire audio.

Feature 11 adds USB ingress, source discontinuities, packet timing, controller
events and the selected USB alternate setting without changing features 5 or 9.
Feature 12 changes only the temporary diagnostic scan pause; normal microphone,
pairing and suspend policy still applies. It is intended for bounded A/B captures,
not as a persistent playback preference. Neither command edits hearing settings.
The exact layout and capture commands are in [audio continuity](AUDIO-CONTINUITY.md).

## Case BLE battery observations

Firmware 0.1.11 can supplement status-report batteries from a separate case
advertisement. It requires the selected pair's advertising key from a complete
AACP key response and valid Pro 3 model metadata (A3063/A3064/A3065). Keys remain
in RAM, survive same-pair AACP loss and clear on pair change or invalid replacement.
No key is supplied by a host or included in USB/RFCOMM responses.

The bounded parser requires exactly one Apple type `07`, length `11` record:
`07 11 06` followed by one AES-128-ECB block. Decrypted bytes 0–1 must be `29 20`;
bytes 8, 10 and 11 must be zero, and byte 2's upper nibble must be zero. Byte 2
contains the lid bit (8 means closed) and three-bit open counter. Bytes 3, 4 and 5
are case, left and right battery: `FF` means absent; otherwise bit 7 is charging
and the remaining percentage must be 0–100. Variant bytes 6–7 and counter bytes
12–15 remain opaque; changes, resets and counter decreases are accepted.

An identical last encrypted block cannot renew battery timestamps, even after
address rotation. This is repeat suppression, not replay protection. A changed
block updates present components; absence preserves the previous percentage and
its timestamp while clearing presence and charging. Charging expires after 90
seconds. Valid percentages from healthy AACP components and newer AACP
observations, including unavailable values, take priority.
Case data never modifies connected AACP readback or the connection generation.
BLE reception, case observations and AACP batteries share an extended monotonic
clock. Long absences cannot reverse their ordering or revive expired charging;
the 32-bit status ages saturate below the reserved unknown value.

The shared USB/phone status report exposes presence at byte 44 and case provenance
at byte 45 only with capability 64. Use an updated app to interpret retained
absent values. Association is a keyed plaintext-layout check, not authenticated
identity or evidence of audio ownership. It is excluded from nearby finding,
automatic reconnect, pairing, ear detection and sound controls.

Layout evidence comes from [stuffz/airpods' Pro 3 observations](https://github.com/stuffz/airpods/blob/8bd035451668388860e62fcc0a4a624a608d9d3a/docs/ble-research.md).
Our decoder and fixed-key synthetic fixtures have offline regression coverage;
the Pico's case reception, cadence and physical advertiser remain unverified.

## Nearby snapshot (24 bytes)

Bytes 0–3: version 1; state (0 stopped, 1 listening, 2 heard, 3 old reading,
4 paused, high bit `80` indicates selected-pair identity available); latest RSSI
as signed byte (`7F` unknown); trend (0 unknown,
1 stronger, 2 steady, 3 weaker). u32 fields at 4, 8, 12 and 16 are last-heard
age (`FFFFFFFF` unknown), matched report count, BLE pause flags and finder
session ID. Signed i32 at 20 is smoothed RSSI in Q8 (`80000000` unknown).
An old reading means over 90 seconds without a verified report, not proof the
device is absent. No metres, direction or anonymous Apple-device fallback.

The firmware obtains its IRK only from a complete, validated key response on the
selected pair's AACP channel. AES address resolution and an Apple proximity
record are both required before admitting RSSI. No host-supplied key is accepted.
The eight-entry raw BLE cache is a separate, unassociated inventory; its Apple
advertisements cannot become finder results without that identity check.
Median filtering followed by smoothing produces signal and recent trend.
The key stays in RAM and never appears in USB or phone responses. The finder
stops when its owner disconnects, its view closes,
its 30-second polling lease expires or the selected pair changes; an AACP-only
disconnect does not discard the selected pair's cached identity. Poll at least
once per ten seconds while the view is open. HQ microphone and existing scan
pause policies still apply.

The cache's offline type `0x12` reports are not admitted to nearby finding. Case
battery information does not establish a separate case transmitter. The finder
does not enroll Find My, ring a case, pair, acquire audio or change hearing data.

Ear detection uses the existing cached report slot 5. Both apps retain their
existing player-specific pause ownership; stale reports, reconnects and manual
playback changes cannot authorize an unrelated player to resume. USB/phone input
ownership in the profile status also revokes the other app's automatic pause.
Firmware sends no global media resume on ear reinsertion.


## AAC-ELD signal inspection (typed feature 13, firmware 0.1.22)

Request is one byte, 13. Response is exactly 400 bytes, little-endian. It is a
read-only opt-in diagnostic, unsupported for other codecs. Header bytes 0..3:
version 1, codec 3 (AAC-ELD), flags (bit 0 AU truncated, bit 1 capture active), ASC length 1..8. Offset 4 is
snapshot microseconds (u32). Fourteen u32 values begin at offset 8, in order:
USB sample count; USB nonzero samples; latest USB packet peak Q15 (0..32768);
last nonzero USB microseconds; encoder-input sample count; encoder nonzero
samples; latest encoder-input peak Q15; last nonzero encoder microseconds;
submitted AU count; last submitted AU microseconds; actual last AU byte length;
encoder generation; last AU sequence (12-bit); last USB packet microseconds.
Counts are interleaved samples, not stereo frames; all u32 counters/times wrap.

Offset 64 is the RTP timestamp (u32), 68..75 the ASC padded to eight bytes,
76 malformed submitted-packet count (u32), and 80..399 the last AU, padded to
320 bytes. A longer AU sets the truncation flag and is unsuitable for decoding.
Only an accepted transport submission updates the AU; malformed packet framing
increments its error count without replacing the previous good snapshot.
Neither an accepted send nor this snapshot proves receiver delivery. Inspection
includes a short segment of program audio; default counters omit it.

Double-pressing BOOTSEL now requests the opposite Game Mode through the same
live control path as typed feature 4. On acceptance, enabling displays two finite
LED pulses; disabling displays three seconds off, then restores the underlying
connected/off state. It does not block the audio worker or write pairing flash.
From 0.1.39, power-up starts in normal mode; a double press enables Game Mode.

Typed feature 14 takes two bytes, 14 and enabled (0/1), and returns no payload.
Detailed inspection starts disabled. Enable/renew grants a 120-second capture
lease; disable ends it immediately while retaining the latest snapshot. The
collector's --inspect option enables it, renews every ten seconds, and disables
it on exit. Expiry also bounds overhead if the collector disappears. Feature 13
itself remains read-only. Normal playback does not scan sample arrays or cache
codec frames, and periodic BLE status printing and BTstack information-level logging are disabled; CDC timing prints
occur only on an explicit diagnostic request.

## Phone input counters (typed feature 15, firmware 0.1.27)

Request is one byte, 15. Response is little-endian: version 1 is 52 bytes with
12 fields; version 2 (firmware 0.1.32) is 64 bytes with 15 fields. Header bytes
0..3 are version; codec (0 none, 1 SBC, 2 aptX, 3 LHDCv5); state flags; field count.
Flags are signaling connected (1), configured (2), opened (4), started (8),
Start pending (16), Start accepted (32), worker ready (64), and AirPods output
ready (128).

Twelve u32 values start at offset 4, in order: accepted packets, malformed
packets, sequence gaps, worker submission failures, decoder failures, buffer
resets, published stereo PCM frames, latest media-packet milliseconds, queued
packets, decoded output waiting (0/1), latest decode microseconds and maximum
decode microseconds.

Version 2 appends three u32 values at offsets 52, 56 and 60: maximum active
validated-media arrival gap in milliseconds; maximum nonempty decoded-PCM
publication gap in milliseconds; RTP timestamp-discontinuity count. The latter
checks contiguous RTP/codec sequences on the same SSRC against the previous
packet's frame duration, and saturates at UINT32_MAX. Raw aptX has no RTP check.
Idle, reset and genuine route loss disarm timing comparisons; scoped recovery
remains eligible. Maxima/counts persist across those boundaries until firmware
initialization. A publication gap can include decoder or main-loop delay and
does not by itself identify CPU saturation. The collector supports both layouts.

Other packet/error/reset/frame totals wrap at 32 bits and persist until firmware
initialization. Resets include ordinary start/stop transitions, so compare
steady-playback deltas. Timing and queue values describe the current worker.
In 0.1.27, queue-full submissions also incremented malformed/reset totals and
cleared PCM. From 0.1.28, congestion increments only submission failures; valid
queued work is retained and decoder history resets at the subsequent accepted
packet boundary. That ordered codec reset does not clear PCM or increment the
buffer-reset field. The wire layout is unchanged.
The snapshot neither resets counters nor changes playback. The collector's
`--phone` option requests it without enabling audio-payload inspection or logging.
