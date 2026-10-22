# AAC-ELD playback

This describes the firmware's selected configuration. USBPods supplied the
original vendor-codec implementation; NTPods supplies the packet-validation,
priming and recovery reference. This revision still needs hardware validation.

## Negotiation

Apple's A2DP vendor codec uses vendor `0x004C` and codec `0x8001`. Its 14-byte
codec information starts with:

```text
4C 00 00 00 01 80   Vendor and codec, little-endian
00 80               AAC-ELD object-type bitmap
00 84               Selected 48 kHz stereo configuration
00                  Reserved
03 E8 00            CBR, 256000 bits/s
```

Capabilities can advertise both channels and the VBR bit. The selected
configuration uses CBR to match the encoder. A lower advertised maximum reduces
the selected rate; rates below 128 kbps are rejected by this implementation.

| Encoder parameter | Selected value |
| --- | --- |
| Audio object type | 39, ER AAC-ELD |
| PCM input | 48 kHz stereo, signed 16-bit |
| Frame length | 480 samples per channel, 10 ms |
| Bitrate | CBR, at most 256 kbps and the peer's ceiling |
| SBR | Off |
| Afterburner | Off |
| Transport | Raw access units |

Changing the SBR setting or advertising VBR requires a separate encoder and
wire check. A host-reported PCM bit depth does not establish the compressed
stream's effective resolution.

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
