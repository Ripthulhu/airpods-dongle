# Dongle and Android protocol comparison, 16 November 2026

Version 0.1.67 incorporates the baseline-preserving Game Mode policy from
LibrePods Pixel145 (`1625f49`). That release retained the pitch-preserving
USB transition. The current preview uses [variable-speed USB playback](GAME-TRANSITION.md).
Transport-specific feature declarations and audio ownership remain
separate; an undocumented flag is not enabled merely because another host sends it.

| Area | Dongle | Android | Assessment |
|---|---|---|---|
| Receiver buffer | AACP 60/150 ms targets, finite 9/11 ms clock correction | Same targets and finite correction | Wire targets agree; USB source handling differs. See GAME-TRANSITION.md. |
| Delay admission | Previously accepted almost any nonzero/non-FFFF report | Rejects a correction above 500 ms | 0.1.61 adds the same correction bound. |
| Controller flush | Retains the normal 200 ms controller timeout in Game Mode | Retains the captured controller baseline, including zero | Same baseline-preserving policy. Debug and Release now select the tested Android policy together. Receiver targets and finite clock correction still change. |
| Controller packet mask | Retains the controller-confirmed startup mask in both Game Mode states | Retains the controller-confirmed mask in ordinary Debug and Release | Neither path adds the earlier `0x2204` exclusions for 3-Mbit EDR. Startup admission, late-reply fences and restoration of existing debt remain. No universal `CC18` mask is assumed. |
| AACP connection | Requests service 4 version 1.3, with one bounded legacy fallback and read-only capability discovery | Requests version 1.3; retries initialization only when the current service has not been accepted | Both distinguish successful local writes from validated peer acceptance. No repeated setup of a healthy connection. |
| Source feature flags | Existing FF mask | Existing D7 mask plus explicit opt-in hearing capability | Preserved per transport; no unverified hearing or Mac source announcement enabled. |
| Hearing host clock | LibrePods supplies one current UTC opcode 0x1B through the existing target-bound settings transport | Same packet builder and once-per-session policy | No Pico uptime-as-calendar substitution. Failed sends do not count as confirmed local transmission. Audible hearing effects remain unverified. |
| Microphone commands | Existing start/stop bytes | Same start/stop bytes | Transport routing and presentation formats differ by platform. |
| Settings discovery | UUID-based Classic ATT discovery | UUID-based Classic ATT discovery | Mechanism agrees. |
| Transparency confirmation | Bounded baseline-aware readback polling | Same bounded polling policy | Policy agrees. |
| Hearing fitting confirmation | Up to five reads, 75 ms apart, while the old fitting remains | Same bounded baseline-aware policy | Writes are not repeated. Malformed, conflicting or stale-owner results fail; both audiograms remain preserved. |

Source locations: dongle `btstack_aacp.c`, `btstack_hci.c`, `playback_adjust.h`
and `btstack_avdtp_source.c`; Android `AACPManager.kt`, `HostIdentity.kt`,
`ATTManager.kt`, `DongleController.kt`, `NativeGameModePolicy.kt` and
`pixel17_game_runtime.cpp`.

The firmware startup retry does not copy Android's raw-hold delegation, country
setup or sensor activation. Those commands require their matching host features;
copying them could interfere with native stem behavior. Neither startup nor a
settings read acquires audio ownership. Button/app takeover remains explicit.

Regression coverage includes delayed fitting commits, changing owners, host-clock
rejection and retirement, accepted/unaccepted startup, controller command deadlines,
stale acknowledgements and per-peer isolation. Build and synthetic checks do not
prove an audible hearing-processing effect or replace hardware listening tests.
No saved hearing profile, volume value or pairing is changed by the merge itself.

The Android-specific improvement also batches guarded native state reads instead
of repeatedly scanning `/proc/self/maps`. Pico firmware reads its own static
state and has no equivalent memory-map scan to remove. Android's pause/status
and foreign-command cleanup repairs remain in its native/app state machines;
they are not copied into the Pico's separate control engine.

The 0.1.67 packet-policy change requires its own firmware listening, Game Mode
On/Off, weak-signal and reconnect checks. Pixel145's accepted phone comparison
does not establish Pico audio stability or measured end-to-end latency.
