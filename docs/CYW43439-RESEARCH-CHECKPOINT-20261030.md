# CYW43439 research checkpoint — 30 October 2026

The controller-firmware investigation is paused at the user's request. No further
controller diagnostics were run after that stop. Both dongles returned to their
normal firmware; final full 4 MiB comparisons matched their pre-test flash images.
LibrePods was reopened. Private device backups are retained locally, not published.

## Preserved baselines and evidence

- [PC baseline v0.1.54-r6-bw20000](https://github.com/Ripthulhu/airpods-dongle/releases/tag/v0.1.54-r6-bw20000): source commit `63bf50857f843456529978733371b18b7241d3f5`.
- [Phone baseline v0.1.57-r5-bw20000](https://github.com/Ripthulhu/airpods-dongle/releases/tag/v0.1.57-r5-bw20000): source commit `14b1043a5d192f041ea32736099fee6860aa408d`.
- [Research source and evidence archive](https://github.com/Ripthulhu/airpods-dongle/releases/download/v0.1.57-r5-bw20000/cyw43439-research-checkpoint-20261030.tgz), attached to the phone baseline release: 4,183,440 bytes, 264 files; SHA-256 `b1623eaaa79864efd213c6a05a108c87a7cc0a516a0bce0f84be22947c450948`.

The archive is evidence, not installable firmware. It contains diagnostic source,
scripts, public input metadata, manifests, reports and logs. It excludes private
folders, credentials, flash backups, experimental ELF/UF2 files, firmware containers
and firmware byte-array headers. Board identifiers in evidence copies are redacted;
the manifest records original and archived hashes. Public inputs can be recovered
from the recorded source URLs and hashes if research is explicitly resumed.

Start with `READBACK-CHECKPOINT.md` and `READBACK-CHECKPOINT.json` in the archive.
The structured record verifies all eight readback transcripts and preserves their
hashes, events, timing and restoration summaries.

## Result

On both boards, both readback versions compare every defined input byte before the
final startup callback: **6,742 stock bytes and 15,648 hybrid bytes**, with matching
CRCs, zero byte mismatches and no reported host write errors. The exact callback
is forwarded once. This verifies readable input before the trigger, not successful
firmware execution or authentication.

Stock initializes and passes HCI Reset / Read Local Version. The transformed
`.0076` hybrid fails the firmware-ready polling branch with result 180 on both
boards. Its readback finishes at approximately 572 ms; initialization returns at
1,026 ms, after another 454 ms. No hybrid HCI controls or SCO test follow that failure.

The last discriminator sampled six fixed direct SPI registers at four points.
All 16 snapshots across stock/hybrid on both boards retain `READ_TEST=0xFEEDBEAD`,
bus control `0x000200B3`, status `0x20`, clock CSR `0xC2`, sleep CSR `0x03` and the
expected `0x18000000` backplane window. Alternate SPI framing is therefore not
justified by the observed failure.

All 18 hybrid post-init backplane words nevertheless read as zero, including ROM
literals that stock reads as `0x002038CC` and `0x00329000`. Those values are invalid
as boot-state evidence. Pico's transfer success code confirms PIO/DMA completion,
not valid target data. The result does not establish an empty patch allocator,
unparsed configuration, signature rejection or a dead SPI interface.

Pinned diagnostic inputs:

| Input | Commit or SHA-256 |
|---|---|
| Pico SDK | `079c6f39023649b154152db30f1d781e884879bc` |
| CYW43 driver | `055d64274b014dd7b1c2fc94d26e8a18face7124` |
| Final diagnostic source archive | `c62cf80fb7bc143ff3a485469f997c6ff8477d20e56fc89c4139bbcbc7c0a586` |
| Stock container | `d974384d0aa8124d30084f30d13519a3ed3b9f759f7ddd6a34ef3dcf1441bcf7` |
| Experimental hybrid container | `e263a05fa2856866e11434551916b7a001de55250419ed39f1d9d4e48c626cad` |

## Proposed next discriminator — not executed

Compare fixed ChipCommon/WLAN backplane anchors with the existing BT ROM sentinels,
checking actual window bytes around selection. The proposed addresses are
ChipCommon base `0x18000000`, WLAN ARM wrapper IOCTRL `0x18103408` and RESETCTRL
`0x18103800`. The WLAN addresses derive from the pinned core/wrapper offsets;
ChipCommon chip-ID semantics still need independent verification. This would help
separate general backplane failure from a BT-domain-specific failure.

**Research stopped before these reads. No domain probe was built or run.** The
hybrid remains an unsuccessful startup experiment, not a usable replacement.

## Separate LED correction

The user subsequently requested a startup LED correction. Both normal builds were
updated with that separate application-only change: PC **0.1.54-r7** and phone
**0.1.57-r6**. Full build/check suites passed, flashing was verified, pairing/settings
storage was preserved, and both production runtimes responded afterward. This does
not resume controller research or change the radio firmware. See the
[LED correction and release links](LED-STARTUP-FIX-20261030.md) for exact source
commits, scope and validation limits.
