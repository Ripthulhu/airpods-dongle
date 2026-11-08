# AirPods command line

Use a checkout of `main`, or [download the CLI ZIP](https://github.com/Ripthulhu/airpods-dongle/releases/latest/download/airpods-cli.zip)
and open a terminal in its extracted `airpods-cli` directory. This repository is
private; GitHub sign-in with repository access is required for downloads.

Run from the repository root with Python 3.10 or newer. The CLI uses the existing
USB HID protocol through Windows' built-in HID driver or Linux/Android `hidraw`.
There are no extra Python dependencies. Use the
[current firmware release](https://github.com/Ripthulhu/airpods-dongle/releases/latest)
for the supported control protocol. Linux/Android requires access to the
dongle's hidraw node.

```powershell
python tools/airpods.py help
python tools/airpods.py --help
python tools/airpods.py help set
python tools/airpods.py help mode
python tools/airpods.py status
python tools/airpods.py connect
python tools/airpods.py disconnect
python tools/airpods.py reconnect-policy
python tools/airpods.py reconnect-policy manual
python tools/airpods.py settings
python tools/airpods.py get mode
python tools/airpods.py set mode transparency
python tools/airpods.py set ear-detection on
python tools/airpods.py set microphone auto
```

With multiple dongles attached, put `--serial YOUR_PICO_USB_SERIAL` before the
command. The serial identifies the Pico, not the AirPods or a COM port:

```powershell
python tools/airpods.py --serial YOUR_PICO_USB_SERIAL get mode
```

Close NTPods and other HID control clients before using control commands. USB
clients share one transaction channel; a busy result is an error, not permission
to cancel another client's work or retry a write. Serial diagnostics can remain
open because they use a different interface.

## Commands

| Command | Result |
|---|---|
| `help [COMMAND_OR_SETTING]` | Commands, examples and accepted values; never opens hardware |
| `status` | Connection, settings readiness, output codec and cached batteries |
| `pair [airpods\|phone]` | Start new AirPods pairing (default), or open phone pairing for 120 seconds |
| `connect` | Explicitly connect to the saved AirPods and request audio ownership |
| `disconnect` | Release the AirPods and hold automatic reconnect off, keeping pairing |
| `reconnect-policy [manual\|auto]` | Read the reconnect policy, or save a new policy while audio and connection transitions are idle |
| `settings` | All named device settings, with report age or an explicit unknown value |
| `get SETTING` | One reported value and its age |
| `set SETTING VALUE` | Validate, send once and wait for a fresh matching device report |

`get` and `settings` read the device-reported cache; they do not force a new Apple
notification or guess a value. Unknown can mean unreported or unsupported. A
device report's age is shown in milliseconds. Model/firmware checks in the dongle
remain authoritative; listing a command does not mean every model supports it.

## Pairing and connections

For AirPods that are already saved on the dongle, use `connect`, or press the
dongle button once briefly. This is an explicit request to reclaim audio and can
interrupt another host. It does not replace the AirPods bond.

For new AirPods, pause playback and microphone use first. Leave both buds in the
open case and put the case in pairing mode (flashing white), then run:

```powershell
python tools/airpods.py pair
```

If AirPods are already saved, the CLI refuses to erase them unless you explicitly
use `pair airpods --replace`. That command disconnects the selected AirPods,
clears their saved bond and starts discovery. Use it only to replace or redo
pairing. A successful response says pairing **started**, not that the AirPods
have already paired. Use `status` to check the eventual connection.

To pair a phone to the dongle's Bluetooth relay, stop playback and microphone
use, disconnect any existing phone relay connection, then run:

```powershell
python tools/airpods.py pair phone
```

This opens a 120-second phone pairing window. Select **AirPods Dongle** in the
phone's Bluetooth settings. It does not clear the saved AirPods bond or promise
that phone pairing has completed. `--replace` does not apply to phone pairing.

`disconnect` releases the AirPods without erasing pairing. Automatic reconnect
remains held off until an explicit `connect`, a dongle button press or reboot.
It also establishes that hold if the saved AirPods are already disconnected.
Incoming connections from the AirPods do not override the hold.

New firmware confirms `connect` using the matching routing receipt and actual
audio ownership, and waits for `disconnect` to finish closing the connection.
Accepted requests can take several seconds to finish. The CLI stops waiting if
the receipt is replaced, the device disappears or completion times out; it never
resends the request. On older firmware without routing support, `connect` uses
the existing reconnect command and reports only acceptance; `disconnect` is
unsupported, with no pairing or other fallback.

## Reconnect policy

The default `manual` policy avoids grabbing AirPods from another device. The
dongle waits for an explicit Connect/button request or a connection initiated
by the AirPods themselves. Read the policy separately from ordinary status:

```powershell
python tools/airpods.py reconnect-policy
python tools/airpods.py reconnect-policy manual
```

`reconnect-policy auto` is an explicit opt-in to automatic outgoing reconnects.
It can reclaim AirPods that another host is using. The change takes effect on
subsequent eligible attempts, which may happen immediately. A previous
`disconnect` hold still overrides Auto until Connect, a button press or reboot.
The policy is retained across restarts; the temporary disconnect hold is not.

Pause playback and microphone use, wait for connection changes to finish and
close competing control clients before changing the policy. Firmware refuses
changes when flash storage cannot safely be updated. A successful save verifies
the stored value before acknowledging it. An unchanged policy sends no write;
the initial default Manual value does not require a saved record.

Policy commands require new firmware. Unsupported firmware or an unreadable
saved configuration returns an explicit error without changing the policy.
An unreadable configuration fails closed to Manual; inspect the firmware/storage
state or restart before deciding whether to try again. Ordinary `status` does
not query policy or use the shared transaction channel.

## Setting values

| Setting | Values |
|---|---|
| `mode` | `off`, `anc`, `transparency`, `adaptive` |
| `microphone` | `auto`, `left`, `right` |
| `ear-detection`, `one-bud-anc`, `conversation-awareness`, `personalized-volume` | `on`, `off` |
| `allow-off`, `pause-when-asleep`, `case-sounds`, `optimized-charging`, `volume-swipe` | `on`, `off` |
| `press-speed`, `hold-duration` | `default`, `slower`, `slowest` |
| `swipe-duration` | `default`, `longer`, `longest` |
| `call-controls` | `default`, `swapped` |
| `crown-direction` | `default`, `reversed` (supported headphones only) |
| `tone-volume`, `case-volume`, `adaptive-strength` | integer `0` through `100` |
| `hold-modes` | comma-separated selection of at least two distinct listening modes |
| `hold-actions` | two comma-separated actions, left then right: `noise-control` or `siri` |

```powershell
python tools/airpods.py set hold-modes anc,transparency,adaptive
python tools/airpods.py set hold-actions noise-control,siri
python tools/airpods.py set tone-volume 75
```

The hold-mode selection is shared by both ears, not an ordered cycle. The CLI
does not silently enable `allow-off` to satisfy another command. `hold-actions`
explicitly sets both sides together. Tone/case volume refers to notification
sounds, not music volume. No raw ATT packets or hearing-profile edits are exposed.

## Confirmation and exit codes

A successful USB transmission alone does not confirm a device change. For `set`, the CLI
waits for the requested value with a different report revision in the same
connection generation. A reconnect invalidates the operation. If the cache
already reports the requested value, the CLI prints its age and sends no setting
write; that is distinct from a newly confirmed change.

| Exit code | Meaning |
|---|---|
| `0` | Read/help succeeded, change confirmed, already reported, or pairing/legacy connection request accepted as described in its output |
| `1` | Connection/protocol failure, unknown requested reading, or rejected operation |
| `2` | Invalid command or value |
| `3` | Write may have applied, but confirmation was lost or timed out |

After exit `3`, inspect `get SETTING` and its report age, `status`, or
`reconnect-policy`, depending on the command, before deciding to try again.
Pairing setup can fail after disconnecting or changing a bond, so a failed Pair
receipt is not permission to repeat it blindly. The CLI never retries a write
or reopens another dongle after an uncertain result. Only an explicit `connect`
requests audio ownership; ordinary reads and settings commands do not connect
or change playback volume. Firmware blocks setting writes during microphone use.
