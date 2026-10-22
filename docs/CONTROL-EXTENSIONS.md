# Dongle control extensions

Firmware 0.1.8 extends the [version-1 USB reports](USB-CONTROL.md) with phone
controls, device settings, output profiles, Game Mode, performance readings and
nearby finding. The implementation and host tests are complete; Pico/AirPods
hardware verification is pending. Audio ownership is unchanged. No PC/phone
handoff commands are added.

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
| 1: device setting | control ID u8, value u32 little-endian | empty; readback is separate |
| 2: settings snapshot | none | cached setting records below |
| 3: audio profile | output codec u8 (0 auto, 1 SBC, 2 AAC, 3 ELD), rate mode u8 (0 CBR, 1 VBR), bitrate u32 in bits/s | profile status below |
| 4: Game Mode | enabled u8 (0/1) | 20-byte profile status; device confirmation is separate |
| 5: performance | none | versioned diagnostic snapshot |
| 6: start nearby finder | none; identity comes from the selected pair's AACP key response | finder snapshot |
| 7: nearby status | none | finder snapshot |
| 8: stop nearby finder | none | empty |
| 9: audio profile status | none | profile status below |

Cached report slot 6 also returns the settings snapshot. Format: version u8=1,
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
`20`, `36`, `3B` settings. Availability follows the reported model, firmware and
control readback; this is not a promise that every model supports every control.
Ear detection reverses the ordinary toggle encoding: `0A` uses 2 on/1 off;
ordinary toggles use 1 on/2 off. Microphone preference is 0 automatic, 1 right,
2 left. Press/hold/swipe durations use 0–2; values are not shifted to match
older UI indexes. Left/right actions are sent together, preserving the other
side. Existing hearing/diagnostic RPCs and their restoration guards are unchanged.

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

A profile change returns busy during USB/phone playback, phone preparation,
microphone use, pending connection/profile work or an unresolved Game Mode
restore. Pause first. Accepted changes close and reconnect the selected AVDTP
session for fresh negotiation; no codec is reset in a running stream. A complete
current-peer capability catalog can reject an unsupported explicit choice
before disconnecting. Partial, unknown or truncated catalogs require fresh
negotiation. Explicit choices never fall back silently, and pending negotiation
has a 15-second deadline.
Starting headset pairing cancels any pending profile reconnect before discovery.

Startup priming stays fixed at 100 ms. Profiles do not change timestamps,
packet grouping, PCM reserve or flush policy. These controls report transport
configuration, not an acoustic quality or latency measurement.

## Game Mode

The current gate requires a recognized Pro 3, firmware major at least 8,
negotiated AAC-ELD and that endpoint's advertised delay-reporting support.
After 500 ms of active playback, firmware requests the reviewed 60 ms receive
buffer with AACP control `0B`. It restores 150 ms on pause, source loss,
microphone use or disable, retaining a restore obligation across a same-pair
AACP reconnect. Missing confirmations time out rather than retrying forever.
The user preference can remain on while playback is paused.

Queued, sent and confirmed are separate states. Confirmation requires actual
successful command transmission and a new nonzero AVDTP delay report within
two seconds. An old report cannot confirm a new request. This establishes a
transport response, not measured speaker delay or a guarantee that the buds
adopted exactly the requested buffer.

## Audio profile status (20 bytes)

Bytes 0–3: version 1, requested codec, actual codec, rate mode. Codec values are
0 unknown/automatic, 1 SBC, 2 AAC, 3 ELD. Requested and actual bitrates are u32 at
4 and 8. Byte 12 is the Game Mode preference; byte 13 is its state: 0 off,
1 pending, 2 sent/unconfirmed, 3 confirmed by a fresh delay report, 4 failed,
5 unsupported, 6 restoring. Byte 14 is input ownership: 0 idle, 1 USB, 2 phone.
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
