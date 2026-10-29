# Firmware re-audit and 0.1.43 changes

What this repo does: a Pico 2 W receives USB or phone Bluetooth audio, sends
encoded audio to AirPods, and bridges media controls and calls. Expected load is
one phone, one AirPods pair, optional microphone use and bounded diagnostics.
The baseline is `163d46f` (0.1.42). Three parallel reviews covered audio work,
connection/control recovery, and phone codecs/call/decoder lifetimes.

## Must fix

No additional must-fix defect was confirmed in this pass. Sustained listening
acceptance of the 0.1.42 scheduling fixes is still open; a successful build or
sample-equivalence test cannot establish that the reported Bluetooth hitches
are resolved. See [the installed baseline](VALIDATION-0.1.42.md).

## Should fix

1. **An audio takeover could discard the old phone's Pause command.**
   - What: Losing AirPods audio ownership queues a phone AVRCP Pause.
   - Problem: The following media teardown cleared that queue before it ran.
     Moving the queue poll earlier alone would still lose Pause when AVRCP was
     busy with another command.
   - Fix: Stop accepting audio immediately, give Pause priority over old queued
     controls, and retain the old media profile until its press/release command
     completes or the existing two-second control deadline expires. Peer,
     channel and session checks prevent deferred work crossing connections;
     explicit USB microphone/call preemption still releases immediately.
     Canceling an already-accepted but unfinished SDK command retires that
     exact control channel and fences fresh Start until it closes or completes.
     Independent review reproduced a late Pause without this extra fence.
     Transport completion is not proof that every phone player obeyed Pause.
   - If skipped: The old phone can continue playing during a takeover.

2. **Automatic USB Pause used a Play/Pause toggle.**
   - What: Ownership loss automatically asks the USB host to pause.
   - Problem: Consumer usage `0xCD` can resume a player that is already paused.
   - Fix: Use explicit Consumer Pause `0xB1` for automatic ownership loss.
     Intentional stem presses retain Play/Pause. There is no toggle fallback.
     Cancel queued automatic Pause when ownership returns, and expire it if the
     USB endpoint stays busy. A separate reproduction caught a delayed Pause
     otherwise reaching the resumed player more than five seconds later.
     Host support remains a limit: Android's generic input mapping may treat
     this usage as Break, so it may ignore automatic USB Pause. Reliable
     cross-platform USB auto-pause needs host support, not a guessed toggle.
     The mappings are in [Linux HID input](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-input.c)
     and [Android Generic.kl](https://github.com/aosp-mirror/platform_frameworks_base/blob/main/data/keyboards/Generic.kl).
   - If skipped: Switching devices can unexpectedly start the old USB player.

3. **SBC startup accepted a payload that could never carry a frame.**
   - What: Outgoing SBC uses one header byte plus complete encoded frames.
   - Problem: A positive 100-byte payload was accepted for a 118-byte frame.
     Playback started, but no packet could drain. This is separate from the
     active AAC-ELD hitch investigation.
   - Fix: Reject setup unless the bounded media payload fits one complete frame
     plus its header. Reject invalid encoder frame lengths through the existing
     disconnect path. Production-source tests cover the exact boundary and
     invalid lengths; the old implementation fails the new regression.
   - If skipped: An unsupported SBC negotiation can leave a silent stream.

## Nice to have

4. **Unity-rate resampling repeated identical filter calculations per sample.**
   - What: At exactly 1:1 rate, fractional phase is constant across the block.
   - Problem: The scalar loop recalculated the same interpolated coefficients
     for every frame, or repeatedly selected the same direct-delay branch.
   - Fix: Calculate the fractional filter once per block and hoist the direct
     zero-phase path. Keep all non-unity processing, delay, sample ordering,
     float storage and final rounding/saturation unchanged. The measured change
     adds 136 bytes of transient stack and no persistent storage.
   - If skipped: Output stays correct but the unity render path does unnecessary
     work. This optimisation does not change radio capacity or scheduling.

5. **Idle USB service and main-loop polling wake the processors unnecessarily.**
   - What: USB service ran every 500 microseconds and the main loop every
     millisecond even with no open audio endpoint or active audio work.
   - Problem: Both cores already use the SDK's event sleep; adding another sleep
     instruction would not help. The timer interrupts and alarm events keep
     waking them. A phone left powering the dongle pays for this unused work.
   - Fix: Service USB every 4 ms when both audio alternates are closed and no
     CDC terminal is attached. Keep 500 microseconds for either open audio
     alternate, even during suspend, and restore it in the same callback that
     handles SET_INTERFACE. Slow the fully idle main-loop period to 5 ms; keep
     the existing 1 ms period for streaming, audio transitions, calls, pending
     microphone/decoder work and a connected diagnostic terminal. Snapshot the
     decision under the Bluetooth lock. Button sampling and reconnect timers
     remain available. First main-loop preparation can take up to 5 ms after an
     idle wake; USB audio interrupts still run at their fast cadence.
   - If skipped: The idle USB timer alone continues firing 2,000 times per
     second instead of 250; the idle main-loop target stays 1,000 instead of 200.
     These are service rates, not measured battery-saving percentages.

## Measured optimisation

Cortex-M33 Release code (`-O3`, softfp) was run in the existing QEMU instruction
clock harness. For ten 480-frame blocks, fractional-unity work fell from
63,833 to 45,163 ticks (29.3%); zero-phase work fell from 6,241 to 4,621 (26.0%).
Three non-unity cases changed by zero to two ticks. These are emulated instruction
work measurements, **not Pico elapsed time, CPU cycles or whole-firmware gains**.

Output matched the original across all 655,360 fractional phases, tested block
sizes of 1, 128, 480 and 1,024 frames, and nonfinite/saturation inputs. Both native
sanitizer and ARM-compiled comparisons passed. Existing spectrum and live rate
transition checks also passed. No clock, buffer-size or radio-policy change was
made to obtain the improvement.

## Review coverage

The phone/call review checked packet lengths, decoder mailboxes, cross-core
teardown, SCO send-buffer error handling and pending connection ownership.
Six focused sanitizer suites passed, including concurrent reset/detach/read
tests and ten simulated minutes of microphone clock mismatch. The pinned SDK's
advertised 1,017-byte incoming media MTU fits the 1,024-byte codec storage, and
its failed SCO sends release their reserved buffer. Neither suspected issue
justified a change. CDC command boundaries and diagnostic work limits were also
reviewed without a new confirmed defect.

## Idle-power limits and next measurement

The dongle still draws power while waiting for USB and Bluetooth activity.
Continuous silent USB packets remain an active stream; an open audio alternate
retains the fast service rate. A connected CDC terminal also deliberately keeps
the fast cadence, so current measurements must close continuous serial logging.

Radio settings are unchanged. The pinned SDK asserts Bluetooth's host wake
request during SPI transactions without a matching clear on release; host-ready
is a separate bit. That is a candidate for an isolated, measured sleep/wake
experiment, not proof that clearing it arbitrarily is safe. Unanswered Classic
pages can also last 15.36 seconds with up to 8 seconds between retries, while
passive BLE scanning uses a 10% duty cycle. Changing those policies affects
reconnection and finding/battery updates.

Measure input current with buds closed and disconnected, then connected and
paused, before and after the idle-cadence changes. Keep the same phone, cable,
radio state and logging conditions. Verify cold USB enumeration, reopening
S16/float playback and the microphone, case return, stems, takeover and incoming
call wake before treating power work as hardware-accepted. No current or phone
battery-life saving has been measured in this pass.

## Verdict and limits

Final offline validation passed: all 60 C sanitizer checks, 63 Python tests
(one optional skip), real-SDK volume and phone-codec tests, and LHDC safety
checks. Both Release variants passed the emitted USB-descriptor and linked-memory
checks. Float input still advertises IEEE float32 stereo at 48 kHz with a
392-byte endpoint capacity; S16 and microphone descriptors remain intact.
The float image retains 254,352 bytes of heap and 66,312 bytes after the measured
worst codec allocation, above the unchanged 65,536-byte reserve. S16-only retains
67,096 bytes after that allocation. No limit was weakened to pass these checks.

Build source snapshot: `6416c3efece64ccb0e46effb1f585918d461b1512c9d0cd08062b45678ff575b`.
Float UF2: `557159176e306c4475930c2b689981639682587bcda406c52611ddc97142882a`.
S16-only UF2: `95cd8c95c1e523898822aa44fef88f97b8f1b810bd080bbf33d6a6b6d2e05bcd`.
Final documentation was updated after the snapshot; code, configuration and test
hashes were checked against it before publishing. Reproduction logs, independent
reviews and instruction measurements are retained in lab project
`airpods-pico-bringup-20261027`, including `build-receipt-0.1.43-r2.json` and the
`audit43-*` evidence artifacts.

The changes are narrow: three reproduced control/setup faults, one measured,
sample-equivalent audio optimisation and conservative idle service pacing.
They add no dependencies or persistent
configuration. The codec, packet grouping and Game Mode default remain unchanged.

Not checked in this pass: live listening, actual Pico render duration/current, phone
calls, host-specific automatic Pause, the premature Android maximum-volume cue,
or the earlier radio-flush failure. Hardware remains on the installed 0.1.42
baseline until a separate update; this audit is not a new listening acceptance.
