# libfreeaptx

Unmodified library/header and license from
<https://github.com/regularhunter/libfreeaptx>, revision
`476e2d56b0374c643c50bc37ab5cfee3012e2144` (0.2.2).
Hashes are recorded in the repository's `SOURCES.json`.

The dongle uses classic aptX decoding only. The upstream source also contains
an encoder and aptX HD routines; unused functions are discarded by the linker.
This does not implement aptX Low Latency or Adaptive.

This library is LGPL-2.1-or-later; `COPYING` and all source notices are retained.
The complete dongle source and documented build provide the material needed to
modify this library and relink the firmware. Include this license and the source
when distributing firmware, and do not add restrictions on installing a modified
firmware or reverse engineering it to debug modifications.
