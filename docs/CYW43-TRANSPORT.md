# Shared CYW43 transport repair

The pinned Pico SDK 2.3.1 bus wrapper discarded its queue-full return value.
BTstack could therefore advance a packet and its credit accounting without the
packet reaching the controller's shared-memory ring. The two hash-checked CYW43
patches propagate that result and use BTstack's asynchronous ownership contract:
one existing outgoing buffer remains reserved until the bus accepts it. A timer
retries a full bus while allowing receive traffic and the event loop to progress.
Completion is emitted once after commit; close cancels pending work. Unexpected
transport errors fail explicitly. A queue that stays full for two seconds also
fails explicitly, retaining the unsent buffer without emitting completion. The
application's existing eight-second watchdog is armed before HCI power-on and
then resets the controller and host;
the radio is initialized from scratch. Transient fullness still retries normally.
No extra audio queue is added.

Application controller-command trackers keep their late-reply fences after the
one-second error indication. If a packet-policy, flush-timeout or radio query
still lacks its terminal reply after 35 seconds, they enter the same watchdog
recovery path. Reconnects, mode changes and stopping diagnostics cannot extend
that deadline. This grace exceeds the Bluetooth link manager's
[30-second response window](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62/out/en/br-edr-controller/link-manager-protocol-specification.html).
The trackers never clear a timed-out request merely to send an overlapping
same-opcode command; a valid late reply within the grace still completes normally.

This is the common HCI transport, not a phone-relay-only change. Its command,
ACL and SCO packet handling serves USB-to-AirPods playback, phone relay playback,
AACP, ATT, AVRCP, and microphone/call control and voice traffic that uses this
transport. Incoming packets continue to be serviced while an outgoing write is
blocked. Retaining Host Number Of Completed Packets commands also protects the
receive-credit path. The patch is independent of the application audio route
and applies to CYW43 boards using this pinned SDK transport; it does not repair
unrelated Bluetooth controllers or establish every profile's end-to-end behavior.

The production-function regression compiles the real SDK bus helpers, complete
transport, BTstack ACL fragmentation and Host Number Of Completed Packets
producer. It covers repeated full-to-ready transitions, timer-only progress,
callback reentry, close/reopen, ring wrap, the two-second stall deadline (including
clock wrap and a ready queue at the deadline), explicit errors, command receive-credit
payload retention, and SCO payload retention with all outgoing packet types
gated until commit. The SCO case checks transport ownership and receive progress,
not acoustic continuity or SCO delivery deadlines.

Run against an isolated prepared SDK:

```sh
python3 tools/prepare_sdk.py --sdk /path/to/isolated/pico-sdk
python3 tools/test_cyw43_transport.py --sdk /path/to/isolated/pico-sdk --sanitize
```

The primary repository receives only the shared transport patches, their pinned
manifest groups and regression coverage. Its firmware version, codec/volume
behavior and other ongoing application changes are preserved. This port is not
a deployment: no PC dongle is flashed by these changes. Existing phone firmware
and its listening results do not validate a PC build or prove this defect caused
every audible interruption; USB playback, relay, controls and microphone/call
behavior still require hardware validation of the particular built artifact.
