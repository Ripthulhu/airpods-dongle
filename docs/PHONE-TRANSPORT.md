# Phone bridge

```text
Phone -- aptX or SBC --> Pico 2 W -- AAC-ELD / AAC / SBC --> AirPods
```

The phone pairs with **AirPods Dongle** as an ordinary Bluetooth audio output.
The Pico owns the AirPods bond, Apple Device ID, AACP and output codec. Android
needs no vendor-ID modification. Power the Pico externally for wireless use;
an active USB playback or recording stream takes priority over phone audio.

## Pairing and controls

Pair the AirPods first: put them in pairing mode, then hold BOOTSEL for at least
one second but less than three seconds and release. To pair the phone, hold for
three seconds and release, then select **AirPods Dongle** in the phone's Bluetooth
settings within two minutes. The phone bond is saved separately from the two
AirPods slots. Only the selected AirPods and saved phone are accepted outside
that explicit pairing window. A new phone must complete encrypted, bonded access
before its identity is retained.

AirPods stem media commands and absolute volume go to the phone during phone
playback. USB media commands stay on the USB path. Music only: phone calls, HFP,
SCO and a BLE settings interface are not implemented.

## Codec and buffering

- Input is 48 kHz stereo classic aptX where negotiated, with SBC fallback. The
  dongle prefers aptX during its own discovery; a phone initiating configuration
  can select either supported codec. aptX LL, HD, Adaptive, AAC and LDAC input
  are not advertised. Unavailable aptX allocation withdraws that endpoint so a
  subsequent connection can use SBC.
- Classic aptX contains raw four-byte codewords, without RTP. SBC has RTP and its
  SBC media header. The receiver validates framing and configuration, rejects
  fragmented SBC, and resets on SBC sequence gaps instead of playing stale PCM.
- Decode goes straight into a 64 ms maximum PCM queue. Initial prefill is 30 ms
  for AAC-ELD/SBC output and about 41 ms for AAC; it leaves a small reserve for
  packet arrival phase and clock drift. There is no compressed playout queue.
  A bounded resampling correction follows queue fill to compensate for independent
  clocks. Saturation discards the oldest complete PCM frames. These are buffer
  thresholds, not measured acoustic delay.
- Open the AirPods output before accepting the phone's Start. AAC-ELD's mandatory
  100 ms of encoded silence must be sent successfully before that acceptance,
  so phone audio cannot accumulate behind startup priming. Readiness has a
  three-second deadline; failure rejects Start rather than claiming playback.
- Phone AAC-ELD output uses one 10 ms frame per packet. USB retains its existing
  packet grouping. Codec bitrate, output timestamps and controller flush policy
  are unchanged.

The SBC demo's 60–80-frame buffer would hold 160–213 ms at 48 kHz. It is not used.
Small codec frames alone do not establish end-to-end delay: Android's batching,
radio scheduling, resampling and AirPods rendering must also be measured. aptX
to AAC-ELD is still a lossy transcode.

## Memory and ownership

Bluetooth, USB, playback encoding and phone decoding run on core 0. Core 1 owns
the USB microphone decoder. An idle microphone decoder is detached by core 1
before core 0 releases its heap for a phone session. USB recording can initialize
it again after the phone stops. Codec allocation/free stays in thread context
under the Bluetooth lock; no packet callback allocates memory.

The phone and AirPods use independent AVDTP and AVRCP connection IDs. Shared
AVDTP discovery is filtered before the sink framework sees AirPods events.
Two Classic links fill the controller connection pool; passive BLE scanning uses
no connection slot. A connected BLE case would require a separate capacity review.

## Validation and remaining checks

Native sanitizer tests cover negotiation, framing, stale events, pairing-window
ownership, startup gating, bounded queues, controls and decoder release. Eighteen
ten-minute clock simulations cover the three output frame sizes at ±300 ppm,
including fixed-packet arrival drift. Real
aptX/SBC codec tests run natively and in ARM32 emulation. Firmware cross-compilation
checks the pinned SDK API and RAM layout. These checks do not establish radio
reliability, CPU headroom or acoustic latency on the Pico.

On hardware, first verify simultaneous phone/AirPods links, negotiated codec and
clean stereo. Measure first sound and steady delay separately, queue occupancy,
drift and heap/stack use. Then test long playback, pause/resume, phone radio loss,
AirPods case return, AVRCP/volume, USB takeover and microphone recovery. Compare
aptX with an SBC-only phone and check passive BLE reception alongside both links.

Sources: [pinned BTstack AVDTP](https://github.com/bluekitchen/btstack/blob/501e6d2b86e6c92bfb9c390bcf55709938e25ac1/src/classic/avdtp.c),
[pinned sink example](https://github.com/bluekitchen/btstack/blob/501e6d2b86e6c92bfb9c390bcf55709938e25ac1/example/a2dp_sink_demo.c),
[AOSP classic aptX transport](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/system/stack/a2dp/a2dp_vendor_aptx.cc),
[pinned aptX decoder](https://github.com/regularhunter/libfreeaptx/tree/476e2d56b0374c643c50bc37ab5cfee3012e2144).
