# Audio implementation comparison — 2026-10-29

Compared our `5a7f5cd5b207e1633f25fa9eb59bcdef449c0b10` (0.1.52) with
[USBPods `6286ab1`](https://github.com/wasdwasd0105/USBPods-Pico2W/tree/6286ab1618ec1143d828b1fc377d8e22c449e567)
and [picoaudio `5f3e983`](https://github.com/nicx17/picoaudio/tree/5f3e9836318f47c65bdf32a7eceebcaeb5fccbb9).
This was a source review by three independent reviewers, checked against our
capture notes and current firmware. No upstream code has been merged or flashed
as a result of this comparison.

Follow-up: the user subsequently requested both changes. Their implementation
and validation are tracked separately in [0.1.53](VALIDATION-0.1.53.md); the
observations and proposed experiment order below describe the 0.1.52 review.

Our [latest listening run](VALIDATION-0.1.52.md) still hitched after reaching
128 kbps, with no active PCM loss but 15 controller flush events in three
minutes. Lower average bitrate alone has not solved the fault. Two concrete
implementation differences deserve controlled tests.

## 1. Negotiated bitrate permission differs from encoder mode

Our `playback_options_eld()` sets the wire high bit from the local CBR/VBR
selection, and its configuration validator requires them to match. A CBR
256-kbps session therefore selects rate bytes `03 E8 00`. Live adaptation
changes the encoder's rate and peak limit down to 128 kbps without changing
those negotiated bytes. [Our selection and validation](https://github.com/Ripthulhu/airpods-dongle/blob/5a7f5cd5b207e1633f25fa9eb59bcdef449c0b10/src/btstack/playback_options.h#L35-L57),
[live rate update](https://github.com/Ripthulhu/airpods-dongle/blob/5a7f5cd5b207e1633f25fa9eb59bcdef449c0b10/src/btstack/btstack_avdtp_source.c#L1487-L1508).

USBPods always selects variable-rate permission with a 256-kbps field
(`83 E8 00`), including when its Game encoder runs CBR at 192 kbps. Thus its
wire permission and local encoder algorithm are deliberately separate.
[Negotiation](https://github.com/wasdwasd0105/USBPods-Pico2W/blob/6286ab1618ec1143d828b1fc377d8e22c449e567/src/btstack/codec/codec_aaceld.c#L103-L125),
[Game encoder](https://github.com/wasdwasd0105/USBPods-Pico2W/blob/6286ab1618ec1143d828b1fc377d8e22c449e567/src/btstack/btstack_avdtp_source.c#L1214-L1238).

Our existing NTPods protocol notes use that same `83 E8 00` selected template
alongside a selectable CBR encoder. Native October 12 frames 5177/5194 have the
high bit set and a nominal 320-kbps field, while encoder logs report a 192-kbps
target and 256-kbps maximum. Local evidence:
`ntpods/docs/WINDOWS-AIRPODS-AUDIO-PROTOCOL.md` (AAC-ELD selected configuration)
and `ntpods/docs/MACOS-AUDIO-CAPTURE-2026-10-12.md` (AAC-ELD playback comparison).
The nominal field is not a measured stream bitrate.

This is a genuine consistency concern, **not a proven invalid Apple stream**.
The standard AAC CBR/VBR rules are suggestive, but Apple codec `004C/8001` uses
vendor-specific parameters. [A2DP 1.4, sections 4.2.3, 4.5.2.5 and 4.7.2.3](https://www.bluetooth.org/DocMan/handlers/DownloadDoc.ashx?doc_id=544797).
Hitches also existed before adaptation was introduced, so this cannot by itself
explain the original fault.

First compare a fresh manually negotiated CBR 128-kbps stream with the failed
256-kbps session that reached 128 through adaptation. The existing manual
128-kbps ceiling equals the adaptation floor: no firmware change is needed and
live retuning cannot occur. Preserve the Game target, flush timeout, packet
permissions, packet grouping, USB format and volume. Record negotiation and
the actual encoder budget; restore the saved profile afterward.

If fresh 128 is clean, that implicates the fresh profile versus its adaptive
history, not uniquely the wire flag or FDK retune. The next comparison must
separate them: hold actual CBR encoding and its transition sequence fixed while
changing only the negotiated high bit. Selecting the existing VBR profile is
not an equivalent test because it also changes the encoder algorithm.

## 2. Encoding delays Bluetooth background servicing

Our main loop acquires the CYW43 async-context lock before
`dongle_features_poll()`, which calls `dongle_playback_poll()` and ultimately
`aacEncEncode()`. It releases the lock only after that work. An encoding unit
measured up to 6.15 ms in the latest run; the lock therefore delays background
Bluetooth servicing during that work. This proves a scheduling delay, not that
each delay causes audible loss. [Our main loop](https://github.com/Ripthulhu/airpods-dongle/blob/5a7f5cd5b207e1633f25fa9eb59bcdef449c0b10/src/main.c#L550-L610),
[encode call](https://github.com/Ripthulhu/airpods-dongle/blob/5a7f5cd5b207e1633f25fa9eb59bcdef449c0b10/src/btstack/btstack_avdtp_source.c#L1621-L1667),
[SDK background lock handling](https://github.com/raspberrypi/pico-sdk/blob/2.3.1/src/rp2_common/pico_async_context/async_context_threadsafe_background.c#L268-L282).

picoaudio keeps its SBC decoder on core 1: the Bluetooth packet path only
queues compressed frames. This is the useful architectural lesson. It receives
Bluetooth audio for a local I2S DAC; it does not encode AAC-ELD or transmit to
AirPods, so its latency claim is not evidence for our radio path.
[Packet queue and decoder](https://github.com/nicx17/picoaudio/blob/5f3e9836318f47c65bdf32a7eceebcaeb5fccbb9/bt_audio.c#L444-L540).

Our phone/microphone decoder already uses core 1. A smaller candidate for the
outgoing encoder is a main-thread staged job: prepare PCM and reserve output
under the lock, encode outside it, then publish only if the stream generation
still matches. Disconnect, reconfigure, reset and codec-change callbacks must
defer encoder close/set-parameter operations until the job finishes. Otherwise
unlocking around the existing call can use freed state or publish stale audio.
Also verify that steady-state encode does not allocate where an interrupt-side
allocator could preempt it. This design remains unimplemented pending the
narrower profile comparison.

## Other differences and what to retain

| Area | Finding and consequence |
| --- | --- |
| Radio policy | USBPods requests `CC18` packet permissions and a 200-ms controller flush timeout. Our Game policy uses capture-derived `EE1C` and 60 ms. Those Apple requests are evidence, not proof of the best CYW43 policy. Test these separately with fixed encoding if needed. [Packet policy](https://github.com/wasdwasd0105/USBPods-Pico2W/blob/6286ab1618ec1143d828b1fc377d8e22c449e567/src/btstack/btstack_avdtp_source.c#L1675-L1688), [timeout](https://github.com/wasdwasd0105/USBPods-Pico2W/blob/6286ab1618ec1143d828b1fc377d8e22c449e567/src/btstack/btstack_avdtp_source.c#L4377-L4395). |
| Transmit power | USBPods requests a vendor per-link +8-dBm cap; our controller reports current/max 12 dBm. Copying that request is not a demonstrated power increase. [Request](https://github.com/wasdwasd0105/USBPods-Pico2W/blob/6286ab1618ec1143d828b1fc377d8e22c449e567/src/btstack/btstack_avdtp_source.c#L1477-L1490). |
| Packet grouping and CPU | Upstream groups up to three ELD units and also encodes on core 0. We already yield after one unit per main-loop pass and send two in confirmed Game Mode. Importing its fill loop would increase uninterrupted work. [Fill loop](https://github.com/wasdwasd0105/USBPods-Pico2W/blob/6286ab1618ec1143d828b1fc377d8e22c449e567/src/btstack/btstack_avdtp_source.c#L1365-L1395), [core assignment](https://github.com/wasdwasd0105/USBPods-Pico2W/blob/6286ab1618ec1143d828b1fc377d8e22c449e567/src/main.c#L598-L601). |
| RTP and retry | USBPods stamps the next packet using elapsed milliseconds after submission and clears the staged packet even on failed local submission. Retain our exact-packet retry and tested sample clock. Our previous Windows monotonic-ms replay failed; sample-clock replay stayed clear. [Upstream send](https://github.com/wasdwasd0105/USBPods-Pico2W/blob/6286ab1618ec1143d828b1fc377d8e22c449e567/src/btstack/btstack_avdtp_source.c#L606-L645); local `ntpods/docs/WINDOWS-AUDIO-LATENCY.md`, exact captured-AU replay. |
| Clock adaptation | picoaudio switches by roughly 3,906 ppm based on queue thresholds. Our phase-continuous averaged correction is already finer and covered by long integrated tests; importing its controller would be a regression risk. [Resampler](https://github.com/nicx17/picoaudio/blob/5f3e9836318f47c65bdf32a7eceebcaeb5fccbb9/bt_audio.c#L504-L512). |
| Input validation | picoaudio assumes a fixed 13-byte RTP/media header, copies calculated frame lengths into a fixed-size slot without a bounds check, and ignores a failed queue insertion. Reuse the scheduling idea, not this parser. [Queue input](https://github.com/nicx17/picoaudio/blob/5f3e9836318f47c65bdf32a7eceebcaeb5fccbb9/bt_audio.c#L444-L470). |

Do not merge either pipeline wholesale. Preserve float USB input, native AirPods
volume, stem controls, standalone use, default-off Game Mode, reconnect behavior
and the user's fixed phone-case installation. No comparison yet demonstrates
that overclocking, changing Bluetooth firmware, or more transmit power fixes
the hitches. Actual on-air packet choices, retransmissions and receiver arrival
times remain unmeasured.
