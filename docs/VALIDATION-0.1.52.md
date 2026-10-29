# Retain recurring transport losses — 0.1.52

The first adaptive firmware changed bitrate live, but the user still heard
frequent Game Mode hitches on the PC. Its 256 -> 220 kbps change did not make a
clear audible improvement. The evidence and limits are recorded in
[VALIDATION-0.1.51.md](VALIDATION-0.1.51.md).

The policy had required consecutive bad one-second windows. One intervening
clean window forgot the prior loss, allowing recurring isolated discards to
remain at the same bitrate indefinitely. This release keeps that strike until
30 uninterrupted clean seconds. Two bad windows within that period reduce one
step; three flushes in one window still reduce immediately. Even one local
pressure event now restarts the clean-period timer, preventing an upward step
while weak pressure continues. There is no additional state or RAM allocation.

Lifecycle, Game Mode, microphone and ownership changes still clear old evidence.
Urgent lower-only handling for a real transport stall is unchanged. The user
bitrate ceiling, CBR/VBR selection and 128 kbps floor remain unchanged. Radio
sampling and the diagnostic layout remain exactly as in 0.1.51.

Focused sanitizer tests cover separated losses, isolated pressure preventing
recovery, the 29/30-second clean boundary, strike expiry, ineligible resets and
timer/counter wrap. Independent review found no blocking issue. All eight final
build stages passed, including 63 C sanitizer checks, 65 Python tests (one
optional skip), the 14-case real encoder/decoder continuity check, SDK volume,
phone codecs, LHDC safety, float descriptors and linked-memory validation.
The worst emulated allocation leaves 65,944 bytes against the unchanged
65,536-byte reserve. These checks do not measure radio reception.

Source archive SHA-256:
`96f8d7b6d4abaa69641d5437ae71e2d144e765188ef5eb2d6b0232c79509fb93`.
UF2 SHA-256:
`430aaad0717c12e1d94f00ed161614fdd43e33c4780712d51778045546572d4a`.

The user reported that normal mode became clean after the failed 0.1.51 Game
comparison settled. The later normal-mode snapshot capture found the stream
already idle, so it is not a measured normal-mode delivery comparison. It does
confirm that radio polling stops while inactive.

The radio sampler review found no direct ACL-credit starvation: reads consume
command credits and share the outgoing transport briefly. In the 178-second
Game capture, the 18 two-second intervals containing fresh power/AFH reads
included two flush increments; the other 71 intervals included fourteen.
This is not an observer-off experiment. RSSI/link-quality reads updated in
88/89 intervals, so two-second collection cannot separate their timing from
faults. Controller-side observation overhead remains unmeasured.

This corrects the demonstrated adaptation blind spot. It does not establish that
reducing bitrate cures the audible problem. Test the same Game Mode passage,
record actual bitrate decisions and source/radio counters, then repeat the
outdoor phone-case test before claiming improved reliability.
