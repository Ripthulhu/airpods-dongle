# LHDC v5 decoder

Imported `decoder/` and README from
[WillyBilly06/LHDC-V5-Decoder](https://github.com/WillyBilly06/LHDC-V5-Decoder),
revision `cf857ce1a1429965e161e54adbc3f293fd9ce273`.
The upstream README grants Apache-2.0; its License section and a complete copy of
that license are retained. `SOURCE.json` records original file hashes;
`local-changes.patch` records decoder changes.

The adapter accepts only negotiated 48 kHz stereo, 5 ms frames, 16/24-bit input,
at most eight frames and 1,024 compressed bytes per packet. Firmware initially
negotiates 24-bit input; its existing phone worker uses 16-bit PCM.
Prepare/close run after worker detachment on core 0. Decode/reset belong to core 1.
Preparation allocates all transform/window state; failed preparation closes it.
Decode performs no allocation. Logs and duplicate inner-loop clocks are disabled.
The fixed profile keeps 57/63-byte entropy histories, a 1,024-byte payload buffer
and 512-byte channel scratch. Larger table windows and unsupported formats are
rejected; the obsolete pass-2 symbol buffer is removed. Firmware compiles out
upstream trace formatting and its unused latency counters. Upstream host
diagnostic builds retain their larger format bounds and tracing.
No ESP-IDF, FreeRTOS, dual-core decoder worker or upstream host diagnostic path is
included in the port.

Local fixes bound malformed Rice/mantissa values, define integer conversions,
report channel corruption instead of silently inserting silence, and use the
48 kHz normalization derived from AOSP's forward FFT and this inverse transform.
The output scale is `240 * 2^(output_bits-31)`; upstream's approximate per-depth
volume trims are bypassed at 48 kHz.

The small benchmark fixture contains original synthetic signals encoded identically
by pinned C and official AOSP Rust encoders. Its provenance is in
`tests/lhdc/fixture.json`. It is not recorded audio or a Bluetooth capture.
Offline quality checks do not establish Pico deadlines, dual-link radio behavior,
acoustic latency, or decoded-PCM identity with a proprietary reference decoder.
