# Phone transport

Target wireless path:

```text
Phone -- ordinary Bluetooth audio --> Pico -- Apple audio/control --> AirPods
```

The phone pairs with the Pico as a normal A2DP output. The Pico owns the AirPods
pairing, Apple Device ID, AAC-ELD negotiation and AACP. This requires no vendor-ID
change on Android. USB audio remains a separate input; only one input owns playback
at a time. A wireless Pico still needs battery or external power.

## Implementation order

1. Verify the existing USB audio device on Android with OTG and adequate power.
2. Prove two simultaneous Classic links with A2DP sink and source on this radio.
3. Receive SBC from Android, decode to PCM and feed the existing ELD encoder.
4. Route AVRCP playback/volume between phone and AirPods with independent peer
   and connection generations. Keep USB media controls confined to USB input.
5. Add authenticated, bounded BLE settings access for LibrePods.
6. Add phone calls through HFP Hands-Free and SCO separately. Translate call audio
   to the AirPods playback/AACP microphone paths; the phone-side call codec still
   limits fidelity.

The wireless bridge is not implemented. Pinned BTstack has sink/source roles and
shared AVDTP routing, but this does not prove CYW43439 dual-stream reliability.
Two AVDTP connections, another sink endpoint and larger AVRCP/L2CAP/SDP pools are
required. Current HCI capacity is two: phone plus AirPods would leave no connected
case slot. Shared discovery must be serialized, and one role must never deinitialize
or reset the other's shared service.

SBC input avoids another large FDK decoder. SBC-to-ELD adds a lossy encoding stage
and buffering; it cannot improve information already lost on the phone link.
AAC-LC passthrough is a later option only when both peers negotiate compatible
configurations and framing. It would retain AAC-LC rather than produce ELD.

Wi-Fi PCM could serve an app-controlled player, but ordinary Android playback
capture cannot forward every app or phone call. Capture requires user approval,
eligible audio usage and the originating app's permission. It is not a universal
wireless audio output.

## First hardware checks

Keep settings, microphone, codec and scan policy constant during a dual-link music
test. Measure packet loss, drift, queue occupancy and latency separately from CPU
and heap use. Then test AVRCP, radio loss, phone disconnect, USB takeover and BLE
coexistence. Add calls only after stable stereo forwarding; verify call-end recovery
and the full-duplex memory budget independently.

Sources: [pinned BTstack AVDTP](https://github.com/bluekitchen/btstack/blob/501e6d2b86e6c92bfb9c390bcf55709938e25ac1/src/classic/avdtp.c),
[Android playback-capture requirements](https://developer.android.com/media/platform/av-capture).
