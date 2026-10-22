# Phone bridge

Firmware 0.1.8 provides wireless stereo audio and a separate app control service.
Both paths remain untested on Pico hardware.

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

LibrePods connects to the saved phone's encrypted, bonded RFCOMM service for
device settings, hearing profiles, saved checks, audio profiles, performance
readings and nearby finding. See [control extensions](CONTROL-EXTENSIONS.md) for
the UUID and wire format. These controls do not need a PC, a phone vendor-ID
override or an audio stream. The microphone path still requires USB recording.
Opening settings never takes audio; no PC/phone handoff commands are added.

Automatic pause belongs to the app using the active input. It consumes fresh
ear-state reports and resumes only the player it paused, while it still owns
that pause. Disconnect, stale reports, another input or a manual playback change
revokes that ownership. Stored device ear detection is a separate AirPods setting.

## Codec and buffering

- Input is 48 kHz stereo classic aptX where negotiated, with SBC fallback. The
  dongle prefers aptX during its own discovery; a phone initiating configuration
  can select either supported codec. aptX LL, HD, Adaptive, AAC and LDAC input
  are not advertised. Unavailable aptX allocation withdraws that endpoint so a
  subsequent connection can use SBC.
- Classic aptX contains raw four-byte codewords, without RTP. SBC has RTP and its
  SBC media header. The receiver validates framing and configuration, rejects
  fragmented SBC, and resets on SBC sequence gaps instead of playing stale PCM.
- Core 1 decodes into a 64 ms maximum PCM queue via a single completed-packet
  mailbox. Two compressed packet slots bound the cross-core handoff; they have
  no playout timer. Initial prefill is 30 ms
  for AAC-ELD/SBC output and about 41 ms for AAC; it leaves a small reserve for
  packet arrival phase and clock drift.
  A bounded resampling correction follows queue fill to compensate for independent
  clocks. Saturation discards the oldest complete PCM frames. These are buffer
  thresholds, not measured acoustic delay.
- Open the AirPods output before accepting the phone's Start. AAC-ELD's mandatory
  100 ms of encoded silence must be sent successfully before that acceptance,
  so phone audio cannot accumulate behind startup priming. Readiness has a
  three-second deadline; failure rejects Start rather than claiming playback.
- After one second without USB input, phone preparation/playback or HQ microphone
  demand, the output requests Suspend and waits for confirmation. New input
  restarts a locally suspended stream; AAC-ELD resets and repeats its 100 ms prime.
  USB PCM arriving during that restart is retained. Each transition has a
  three-second deadline; phone Start waits until restart and priming finish.
- Phone AAC-ELD output uses one 10 ms frame per packet. USB retains its existing
  packet grouping. Output profiles offer discrete supported AAC/ELD bitrates;
  an explicit unsupported choice never falls back silently. Profile changes
  require idle audio and reopen AVDTP for negotiation. Timestamps, startup
  priming and controller flush policy remain fixed. The
  [profile contract](CONTROL-EXTENSIONS.md#output-profiles) lists valid choices.

The SBC demo's 60–80-frame buffer would hold 160–213 ms at 48 kHz. It is not used.
Small codec frames alone do not establish end-to-end delay: Android's batching,
radio scheduling, resampling and AirPods rendering must also be measured. aptX
to AAC-ELD is still a lossy transcode.

## Memory and ownership

Bluetooth, USB, playback encoding and PCM publication run on core 0. Core 1 owns
phone aptX/SBC decoding or USB microphone decoding; the two inputs are mutually
exclusive. The phone worker allocates 9,772 bytes only during phone sessions.
An idle microphone decoder is detached by core 1
before core 0 releases its heap for a phone session. USB recording can initialize
it again after the phone worker acknowledges detachment and core 0 frees its memory.
Both worker inputs and completed output carry a cancellation generation. A
partial or stale decode cannot enter playback. Codec allocation/free stays in thread context
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

Control tests cover fragmented/coalesced RFCOMM frames, encrypted saved-peer
checks, per-client RPC ownership, replay protection, blocked writers and cleanup.
Finder tests resolve the selected pair's IRK against rotating addresses, reject
other nearby pairs, preserve old-reading ages, and exercise lease/microphone
pause behavior. These are protocol regressions, not live radio verification.

On hardware, first verify simultaneous phone/AirPods links, negotiated codec and
clean stereo. Measure first sound and steady delay separately, queue occupancy,
drift and heap/stack use. Then test long playback, pause/resume, phone radio loss,
AirPods case return, AVRCP/volume, USB takeover and microphone recovery. Compare
aptX with an SBC-only phone and check passive BLE reception alongside both links.

Sources: [pinned BTstack AVDTP](https://github.com/bluekitchen/btstack/blob/501e6d2b86e6c92bfb9c390bcf55709938e25ac1/src/classic/avdtp.c),
[pinned sink example](https://github.com/bluekitchen/btstack/blob/501e6d2b86e6c92bfb9c390bcf55709938e25ac1/example/a2dp_sink_demo.c),
[AOSP classic aptX transport](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/system/stack/a2dp/a2dp_vendor_aptx.cc),
[pinned aptX decoder](https://github.com/regularhunter/libfreeaptx/tree/476e2d56b0374c643c50bc37ab5cfee3012e2144).
