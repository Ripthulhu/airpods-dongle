# AirPods command line

Use a checkout of `main`, or download `airpods-cli.zip` from the
[shared release](https://github.com/Ripthulhu/airpods-dongle/releases/tag/v0.1.57-r6-bw20000)
and open a terminal in its extracted `airpods-cli` directory.

Run from the repository root with Python 3.10 or newer. The CLI uses the existing
USB HID protocol through Windows' built-in HID driver or Linux/Android `hidraw`.
There are no extra Python dependencies and no firmware update is needed for the
shared 0.1.57-r6 release. Linux/Android requires access to the dongle's hidraw node.

```powershell
python tools/airpods.py help
python tools/airpods.py --help
python tools/airpods.py help set
python tools/airpods.py help mode
python tools/airpods.py status
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

Close NTPods and other HID control clients before using settings commands. USB
clients share one transaction channel; a busy result is an error, not permission
to cancel another client's work or retry a write. Serial diagnostics can remain
open because they use a different interface.

## Commands

| Command | Result |
|---|---|
| `help [COMMAND_OR_SETTING]` | Commands, examples and accepted values; never opens hardware |
| `status` | Connection, settings readiness, output codec and cached batteries |
| `settings` | All named device settings, with report age or an explicit unknown value |
| `get SETTING` | One reported value and its age |
| `set SETTING VALUE` | Validate, send once and wait for a fresh matching device report |

`get` and `settings` read the device-reported cache; they do not force a new Apple
notification or guess a value. Unknown can mean unreported or unsupported. A
device report's age is shown in milliseconds. Model/firmware checks in the dongle
remain authoritative; listing a command does not mean every model supports it.

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

A successful USB transmission alone does not confirm a device change. The CLI
waits for the requested value with a different report revision in the same
connection generation. A reconnect invalidates the operation. If the cache
already reports the requested value, the CLI prints its age and sends no setting
write; that is distinct from a newly confirmed change.

| Exit code | Meaning |
|---|---|
| `0` | Read/help succeeded, change confirmed, or already reported with no write sent |
| `1` | Connection/protocol failure, unknown requested reading, or rejected operation |
| `2` | Invalid command or value |
| `3` | Write may have applied, but confirmation was lost or timed out |

After exit `3`, use `get SETTING` and inspect the report age before deciding to
try again. The CLI does not automatically retry, reconnect, acquire audio or
change playback volume. Firmware blocks setting writes during microphone use.
