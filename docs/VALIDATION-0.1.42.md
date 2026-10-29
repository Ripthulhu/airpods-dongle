# 0.1.42 overnight candidate — 29 October 2026

Both lab Pico 2 W boards now run the same float-capable 0.1.42 image. Their
complete pre-update flash was backed up privately, programming was verified, and
the final 64 KiB containing saved pairing data matched byte for byte afterward.
Both rebooted and answered diagnostics. Game Mode is off by default and remained
off in the startup checks. The AirPods stayed in their case throughout this work.

This is ready for listening tests, not a clean-audio acceptance claim.

## What changed

- Fixed the overflow feedback introduced in 0.1.40/41: discarded PCM can credit
  work already overdue, but cannot cancel healthy or future output deadlines.
  Startup losses cannot carry credit into active playback.
- Added cumulative outgoing-time deficit detection. Several short stalls can
  exhaust the receiver even when no single send gap exceeds its reserve. This
  uses the existing bounded, receiver-confirmed Suspend/Start recovery.
- A fresh authenticated phone Start can wake an already-owned suspended output
  after USB/microphone cleanup. Passive profile connection remains passive.
- Reset the AACP retry budget after a verified healthy session and retry an
  AVRCP-only loss without unnecessarily restarting audio.
- Retained the authenticated Bluetooth-session USB mode latch from 0.1.41:
  media retries/profile loss do not restore USB headphones. Full phone Bluetooth
  disconnect restores USB; an existing USB microphone request retains priority.

No CPU clock, codec, buffer-size, scanning or radio-role policy was changed.

## Checks completed

- All 58 C regression checks passed under AddressSanitizer/UndefinedBehaviorSanitizer.
- The Python suite ran 63 tests successfully, with one optional test skipped.
- Real SDK volume, phone-codec and LHDC safety checks passed.
- Release builds passed with float USB enabled and disabled. Both passed the
  linked-memory and emitted USB-descriptor checks.
- The float build retains 254,368 bytes of heap. Worst measured codec allocation
  leaves 66,328 bytes, above the unchanged 65,536-byte runtime reserve.
- The overflow reproduction now sends 48,000 frames during one second of timely
  input, with only the initial 1,344-frame overflow loss. The old logic sent no
  output and lost 48,864 frames. Stereo/sample conservation and priming cases pass.
- The production-source recovery test accumulates eight 40 ms send gaps with
  20 ms packets and requests one recovery after a 160 ms deficit. Catch-up
  repayment, long nominal playback, wraparound, ownership and Game Mode epochs pass.

The source snapshot used for the builds has SHA-256
`017a446beda44f1b04913dcba942b5e06fa360ed138eddef408865d2daa63299`.
The installed float UF2 has SHA-256
`7f356ec977665a7e8cc9464e9ccb56ed0607a993d6ef97095894268dc0d86fc8`.
Final validation documentation was added after this source snapshot; firmware
source and tests were unchanged after the build.

## Silent float USB check

An exact Windows endpoint was opened for 12 seconds in exclusive IEEE float32,
stereo, 48 kHz mode with all-zero samples. Thirteen firmware observations saw
USB alternate 2; their measured interval received 10,758 packets and 516,384
frames, with zero malformed packets and zero short reads. The endpoint returned
to alternate 0 afterward. Persistent Windows format, volume and mute were unchanged.

The buds were asleep, so no AirPods encoding or audible output was tested.
Discarding old startup silence while awaiting the buds is expected and does not
represent active-playback loss. This proves actual float USB input, not the
remaining end-to-end listening acceptance.

Evidence is retained privately in lab project `airpods-pico-bringup-20261027`:
`build-receipt-0.1.42-r1.json`, both memory reports, the two flash receipts,
`audit-float-live-0.1.42-pc.json`, and both idle snapshots. Diagnostic processes
were stopped; continuous packet logging was not enabled. NTPods was already
closed and was left closed. Phone and PC volume settings were not changed.

## Next listening checks

1. Start at a low comfortable volume, with Game Mode off. Connect the Pixel's
   Bluetooth dongle route and confirm USB headphones disappear while that mode
   is selected. Play the same material for at least 30 minutes. Mark any hitch,
   silence or recovery against brief counter snapshots.
2. Check pause/resume and case return. Then compare Game Mode on separately,
   keeping the other variables unchanged.
3. Check Windows and Pixel USB playback, stems/volume and microphone open/close.
   Include Windows float and Symfonium USB-exclusive if available; record the
   actual selected USB format for each path.

Still open: sustained listening on 0.1.42, radio-delivery causes of the earlier
controller-flush capture, full call audio, premature Android maximum-volume cues,
and the separate takeover-Pause/SBC findings in the [audit](AUDIT-0.1.39.md).
