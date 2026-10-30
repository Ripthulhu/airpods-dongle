# AAC-ELD playback

This describes the firmware's selected configuration. USBPods supplied the
original vendor-codec implementation; NTPods supplies the packet-validation,
priming and recovery reference. The validation notes distinguish short PC checks
from the outstanding Android and listening checks.

## Negotiation

Apple's A2DP vendor codec uses vendor `0x004C` and codec `0x8001`. Its 14-byte
codec information starts with:

```text
4C 00 00 00 01 80   Vendor and codec, little-endian
00 80               AAC-ELD object-type bitmap
00 84               Selected 48 kHz stereo configuration
00                  Reserved
83 E8 00            Variable-rate permission, 256000 bits/s wire ceiling
```

Capabilities can advertise both channels and the variable-rate permission bit.
The selected configuration retains that permission when the peer supports it,
independently of the local CBR/VBR encoder choice. The wire ceiling is capped by
both peers; a requested lower local rate does not reduce that permission.
CBR reserves one alignment byte per 10 ms access unit, allowing a 255200 bits/s
local budget under the 256000 wire ceiling. Lower local requests remain unchanged
when they fit. Profile status reports requested and effective local rates; the
wire field is a separate ceiling, not a measurement of encoded throughput.

A fixed-only peer uses fixed CBR at the selected rate, without live adaptation.
That path is also the fallback when variable permission leaves insufficient
headroom for 128 kbps CBR. Actual VBR encoding requires variable permission;
unsupported combinations and rates below 128 kbps are rejected.

| Encoder parameter | Selected value |
| --- | --- |
| Audio object type | 39, ER AAC-ELD |
| PCM input | 48 kHz stereo Q31 after float buffering/resampling |
| Frame length | 480 samples per channel, 10 ms |
| Bitrate | Requested CBR/VBR mode, bounded by the selected wire ceiling |
| Audio bandwidth | Configured CBR ceiling (20 kHz default); ceiling-based VBR |
| SBR | Off |
| Afterburner | Off |
| Transport | Raw access units |

Changing SBR requires a separate encoder and wire check. The variable-rate
permission does not silently enable actual VBR encoding. A host-reported PCM bit
depth does not establish the compressed stream's effective resolution.

## Encoder bandwidth and precision

The following measurements describe the 0.1.53 baseline. Its USB float path
retained float samples through buffering and resampling, then rounded and
saturated to signed 16-bit PCM before encoding. Bit-exact host USB transport
did not establish precision through AAC. See the
[transport quality checks](docs/AUDIO-CONTINUITY.md#transport-quality-checks)
and the 0.1.54 changes below.

For stereo, 48 kHz, 480-sample CBR, FDK's automatic bandwidth table selects
16000 Hz at 255200 bits/s. A separate running-firmware observation on 0.1.53 float r2
confirmed that effective value, with SBR and afterburner off. The encoder applies
the selected bandwidth to its spectrum; this is not a measured acoustic cutoff.
At 128000 bits/s the table selects 15400 Hz; from 192000 through 255200 it selects
16000 Hz. Intermediate interpolation is not strictly monotonic.

With an automatic bandwidth request, live CBR retuning recomputes effective
bandwidth. The 0.1.54 default instead retains its explicit 20 kHz ceiling.
VBR explicitly selects 15500 Hz for a local ceiling
up to 128000 bits/s, 17500 Hz up to 160000, or 20000 Hz above that. This uses the
ceiling, not the current adaptive rate; subsequent bitrate updates retain that
explicit bandwidth until fresh encoder initialization.

For inspection, `AACENC_BANDWIDTH` readback / `aacConfig.bandWidth` is the
effective value. Requested `userBandwidth == 0` means automatic selection.
Requested `userSbrEnabled == 255` is the default automatic sentinel, not an
enabled flag; use `AACENC_SBR_MODE` readback or `syntaxFlags & 0x8000` for effective
SBR state. The observed flags were `0x50` (`AC_ER | AC_ELD`), without SBR.

Wider bandwidth, afterburner, or a higher-precision encoder input are separate
quality changes. Compare encoded output and listening results, worst-case CPU
time, packet-size limits and codec memory before changing defaults. Increasing
bandwidth at the same bitrate also spreads the available bits across more of the
spectrum; it is not an unconditional quality improvement.

## 0.1.54 playback precision and bandwidth

The high-precision USB build retains alternate 1 (S16) and alternate 2
(IEEE float32), and adds alternate 3: signed little-endian PCM32, stereo,
48 kHz, four bytes per sample. This gives Android a standard integer format
instead of requiring its USB HAL to accept IEEE float. The microphone's
S16/64 kHz USB contract is unchanged.

Playback stays in binary32 through the bounded PCM pool and resampler, then
converts directly to Q31 for the 480-frame AAC-ELD encoder. It no longer passes
through S16 first. Binary32 has about 24 significant bits; neither a 32-bit USB
container nor Q31 makes the lossy AAC stream a lossless 32-bit path. Legacy
AAC-LC/SBC still use their S16 input adapters. The FDK library and its microphone
consumer share one coherent Q31 ABI; microphone output is explicitly rounded
and saturated back to the existing S16 contract.

`AACELD_CBR_BANDWIDTH` accepts 0, 16000, 18000 or 20000 Hz; the new default is
20000. Zero retains FDK's automatic bandwidth selection for controlled
comparisons. The existing VBR policy, negotiated bitrate ceilings, afterburner,
SBR state, 480-frame units, packet grouping, queues, priming and clock controller
are unchanged. An explicit 20 kHz ceiling does not guarantee high-frequency
retention at a reduced adaptive bitrate.

The codec memory gate still reserves 65536 bytes for the running system.
The checked R6 build leaves 66064 bytes at the worst emulated codec allocation
peak, only 528 bytes above that reserve. A frame-sized encoder input allocation
preserves LC1024 capacity, while the fixed mono ELD decoder shares sequential
scratch storage with one cleanup owner. Non-profile buffer allocation and
ownership are unchanged.
Do not reduce the reserve or remove microphone/LC lifecycle checks to make a
future build fit. Historical checksum fixtures stay explicitly at automatic
bandwidth; separate cases exercise the configured wider bandwidth.

Exact R6 source SHA-256:
`bd9ef3898397a51cba3ec8310430ea26bfb521b790393754b4d1906f88d0edfc`.
UF2 SHA-256:
`6cf51e92badb13bdc6a663ff40fc7942331ea8da3d23b377c5da898b05ff9ceb`.
The build receipt, independent codec comparisons, and target observations are
under `outputs/dongle-pcm32-quality-20261030` and
`outputs/pcm32-codec-benchmark-r5-20261030` in the coordinating workspace.
PC silent-stream checks verified float USB alternate 2, actual 20 kHz AAC-ELD
configuration, unchanged physical gain 119/127, and no new active-drop,
active-zero, transport-error or recovery counters through short normal and
Game Mode changes. The observed encoder maximum was 6301 microseconds after
the final fresh-connection check; the largest complete encode-unit observation
across normal/Game Mode checks was 8005 microseconds. These
are bounded silent checks, not listening acceptance or acoustic latency
measurements. Android's real alternate-3 playback check remains pending: the
phone dongle's AirPods paging timed out with both R6 and a restored 0.1.53
control image. Its compiled descriptor and codec tests passed; that does not
establish a running Android PCM32 route.

## Packet framing

Each access unit has a four-byte header:

```text
Byte 0: B0 | ((sequence >> 8) & 0F)
Byte 1: sequence & FF
Byte 2: 10 | ((access_unit_size >> 8) & 0F)
Byte 3: access_unit_size & FF
```

The sequence wraps at 12 bits. The implementation caps an access unit at
2047 bytes and also checks the complete header plus payload against the actual
negotiated media MTU. Packets contain at most three access units. The RTP
timestamp advances by 480 per successfully transmitted access unit, using the
inherited 48 kHz sample clock. Failed sends retain the packet and timestamp.

Mac captures reported in upstream research use different timestamp behavior.
Their clock and SSRC choices remain comparison inputs; this firmware preserves
the fork's clock until a controlled radio comparison verifies the whole lifecycle.

## Start and recovery

A confirmed AVDTP Start schedules 100 ms of encoded silence. Source PCM stays
queued in the bounded buffer, followed by the normal playback reserve.
Overflow discards old whole stereo frames rather than replaying a stale backlog.

If a prepared packet cannot be sent for 200 ms, the firmware stops the encoder
pump and requests Suspend. Only a confirmed Suspend permits encoder-history and
sequence reset, followed by Start. A rejected request or three-second recovery
timeout closes the connection. It does not fabricate a successful restart.

The SDK patch keeps AACP and signaling non-flushable. When the controller
supports that distinction, media alone gets a 200 ms automatic flush timeout.
Controller handling of that timeout still needs a USB Bluetooth capture.
