# 0.1.55 AirPods discovery selection

On 30 October 2026, the phone Pico's explicit Pair AirPods command selected an
unrelated discoverable speaker named Power Dynamics (class 0x340404, RSSI
-97 dBm). It saved that address before authentication and repeatedly retried it.
The resulting AVDTP status 102 means a security refusal; status 106 means the
underlying connection ended. HCI reason 0x17 means repeated attempts. These
messages did not establish a problem with the AirPods' own pairing keys.

The old discovery predicate accepted the first rendering/audio-class device.
The connection-result log also said Current Device found before checking for
failure, which must not be treated as evidence of successful inquiry or pairing.
The user reported that the phone dongle had worked earlier that morning.

The repair narrows automatic discovery to compatible Apple audio identities,
with bounded name and previously authenticated identity fallbacks when Device ID
is absent. An explicit non-Apple Device ID is rejected. A saved address without
a pairing key is not trusted. An entirely renamed new device with no Device ID
or authenticated history remains an identification limit.

The PCM32, AAC-ELD bandwidth, volume and audio scheduling paths retain the 0.1.54
implementation. Only the phone board DC1F379C3B1EFCA5 is targeted for deployment;
the PC board is excluded. Firmware deployment uses a full private flash backup,
verified loading and an unchanged final 64 KiB pairing-storage comparison.

The full sanitized regression suite (including the reproduced discovery case),
real adaptive-codec, volume SDK, phone-codec and LHDC checks passed. The firmware
build and linked-memory gate passed with the retained 64 KiB runtime reserve.
The deployment guard's ten mocked checks also passed. An independent review
found no blocker in the Device ID parsing, bounded name checks or fallback.

The phone board was updated from 0.1.54 to 0.1.55 with a full private backup,
verified image, unchanged pairing tail and verified reboot. Its saved AirPods
connection returned with both A2DP and AACP ready and AAC-ELD selected. The user
had independently recovered pairing before the update by walking away from the
unrelated speaker, corroborating the selection fault. Fresh discovery beside
that speaker has not yet been repeated on hardware with 0.1.55.

The Android Quick Settings tile also became available and opened the shared
listening-mode popup against the dongle. No listening-mode or gain writes were
issued during that UI check. Hardware Volume deferred its saved request while
playback resumed automatically, then restored automatically after the user paused.
The final snapshot has Hardware Volume enabled, a fresh physical observation,
active authority and the unchanged gain of 127/127, without a manual gain write.

Frozen source archive SHA-256:
`908e57adc9ab229292e41fe73c2b0bd3d988dfac01db27572e3761fec752dade`.
Verified UF2 SHA-256:
`64dd6a27747dd33b73b6c525e24698d661d26ce03182171437dd2fe292832040`.
The local evidence directory is `outputs/phone-pico-pairing-20261030` in the
parent workspace. No PC firmware update or new listening acceptance was done.
