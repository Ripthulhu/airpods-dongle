# Native Game Mode packet permissions — 0.1.50 candidate

The successful macOS capture changes permitted Bluetooth ACL packet types on
Game Mode entry, in addition to its receiver target and automatic flush timeout.
It requests mask `EE1C` and restores `CC18` on exit. The difference prohibits
3-Mbit/s EDR packet types; it leaves 2-Mbit/s EDR available. Controller acceptance
and Packet Type Changed events confirm both transitions. A second capture
confirms the entry policy. Exact frames and limits are documented in the
[protocol capture audit](PROTOCOL-CAPTURE-AUDIT-2026-10-29.md).

Through 0.1.49 the Pico application omitted that per-link policy. The timeout-only
comparison was acoustically clean despite some controller discards at 60 ms, so
the hitches are still intermittent. This candidate tests the missing packet
permission change separately; it does not change encoded bitrate, audio format,
volume processing, Game Mode target or timeout.

The policy first requests the SDK's locally supported normal mask and
records the controller-confirmed permitted subset. Game Mode prohibits the three
3-Mbit/s EDR families within that baseline. Leaving Game Mode restores the saved
baseline. The policy applies to AirPods links, with acknowledgements matched to
the current connection and bounded failure handling. It is not a global packet
mask for a phone connection. A successful command-status response alone does not
confirm a packet-type change; the matching completion event is required.

## Verification status

Independent review found no blocking issue. The production-handler sanitizer
tests cover the mixed-polarity safe subset, controller-clamped baseline, ignored
DM1 bit, command-credit ordering, delayed replies, missing completion, timer
wrap, changed peers, handle reuse, old live-link restoration and phone exclusion.
Only successful Command Status followed by the matching Packet Type Changed
event confirms a new mask. Lost replies quarantine the opcode instead of
guessing which later connection they describe.

All 62 C sanitizer checks and 64 Python tests passed (one optional skip), along
with SDK volume, phone-codec and LHDC checks. The standard float-capable firmware,
descriptor and linked-memory checks passed. Worst-case codec allocation leaves
66,120 bytes against the 65,536-byte reserve. Frozen source archive:
`cc3cc929632fd06388095e3fba46bf04286523e42fececae4076106e0b301c03`.
Verified UF2:
`9033308c432aaf6bb37ab60781765e87cc8f2c0cab89d0d6a6d7c9bd79bc5016`.

At the user's request, the Pixel-connected Pico was updated from 0.1.46 to
0.1.50 for a mobile listening test. Its full preflash backup was saved on the
phone and copied privately to the PC. Firmware readback passed and the pairing
region matched byte for byte. Startup confirmed normal packet mask `CC18` and
200 ms timeout, with no pending or failed policy command. The AirPods ACL
reconnected; audio ownership still belonged to the other source at that snapshot.

The original PC Pico was subsequently updated from 0.1.49 to the identical
0.1.50 image at the user's request. Its full backup, firmware readback and
byte-exact pairing preservation passed. Startup confirmed normal `CC18` and
200 ms, with no pending or failed policy command. A read-only audio-profile
query reported AAC-ELD, CBR, negotiated 256,000 bits/s and Game Mode off.
Live Game entry/restoration and listening acceptance of the new packet policy
remain pending. The separate Android native
volume bridge APK is not installed by this firmware update. This document does
not claim that the packet policy fixes the hitches or proves which baseband
packet types were actually sent over the air.

## Outdoor field report and adaptation limits

The user reported barely usable audio outdoors while cycling, in both normal
and Game Mode, including two complete disconnections. The route was Pixel to
Pico over USB. The Pico is integrated into the phone case, against the phone,
with the phone in a jacket. The same placement works much better indoors but
still has some hitches. No radio telemetry was recorded during the ride, so this
does not identify Wi-Fi interference, antenna obstruction, power or another
specific cause. The release is not accepted as an outdoor reliability fix.

The firmware offers manual AAC-ELD CBR/VBR profiles and bitrate ceilings of 128,
174, 192, 220 and 256 kbps. Startup Auto means codec/max-compatible selection,
not automatic radio adaptation. Content-sensitive VBR is capped by its selected
peak bitrate; it does not respond to RF quality. Current profile changes require
idle audio and A2DP renegotiation. No live link-adaptive bitrate controller exists.

Radio hopping and retransmission are controller functions. The application does
not currently query the audio ACL's AFH mode/map, RSSI, link quality or transmit
power, nor does it tune AFH or power in response to conditions. Scan/finder RSSI
describes advertisements and cannot substitute for audio-link measurements.
Classic HCI RSSI is relative to the controller's receive-power window, not the
absolute dBm used for LE advertisement RSSI. Controller completion credits also
do not prove successful audio delivery.

The next useful evidence is bounded, opt-in audio-link telemetry alongside
existing source and flush counters, followed by separate placement, phone Wi-Fi
and lower-bitrate comparisons. The Pico antenna should have clear space; nearby
metal can significantly reduce its performance, per
[Raspberry Pi's board documentation](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html).
