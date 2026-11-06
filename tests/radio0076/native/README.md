# Native .0076 behavior proofs

These are the existing instruction-level lab proofs, now self-contained and run
by CI. They compile the **production** C/Thumb sources with the pinned NDK and
execute them with Unicorn. The runner first verifies the unchanged production
adapter rebuild. No device, private ROM dump, captured audio or pairing data is
needed.

```sh
python3 -m venv /tmp/radio0076-tests
/tmp/radio0076-tests/bin/pip install -r tests/radio0076/native/requirements.txt
/tmp/radio0076-tests/bin/python tools/test_radio0076_native.py --toolchain /path/to/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
```

The runner requires all **963 credit cases, 23 inbound groups, 17 outbound groups
and 4 reset/glue groups**. `--output /fresh/directory` retains the logs and proof
reports; by default all build outputs are temporary. An omitted group fails.

- Credit tests execute the repository's unchanged signed .0076 caller, external
  recount/thunk and native report/lookup instructions. Counts, residuals,
  overflow, caller/endpoint guards and register/stack preservation are checked.
- Inbound tests execute native mapping/copy/accounting and the command parser,
  including wrapped headers, malformed input, unavailable pipes and reset calls.
  The reset callback itself is a test stub here; the full implementation is
  exercised by the separate reset suite.
- Outbound tests execute native pipe/list, remapping and ring writer paths.
  Ring-full retry must not consume samples early. A failed commit must retain one
  staging copy, with no second read or metadata free. Epoch loss and cancellation
  preserve native ownership. The ordinary control observer is stubbed in this
  layout and tested with the full production blob in the reset suite.
- Reset/glue tests execute the frozen production adapter, including reset
  cancellation and successful-reset reply matching. Allocator and scheduler
  endpoints are modeled; the native queue predicate and message builder execute.

`rom-fixture.json` contains only **5,088 bytes in 48 spans** of exercised read-only
ROM instructions and literals from source SHA-256
`d5e271f20c80293be3561bb63e92ce4f6ca563ecca8fffd81eba639de9354038`.
The fixture itself is hash-pinned. An emulated instruction or data read outside
these spans fails, except the explicit synthetic test return/callback locations.
Signed vendor code comes from the already frozen `expected_0` array; its hash is
checked independently. Test-only link layouts do not assign production addresses.

The existing outbound proof retains its guarded ten-byte IT-instruction semantic
assist for Unicorn's stale conditional-state issue. IRQ save/restore, clock/wake,
RTOS allocation/scheduling and selected callbacks are modeled as documented in
the individual proof reports. These checks do not prove real interrupt nesting,
worst-case stack headroom, RF reliability or acoustic quality. Fixed 60-byte
CVSD host / 120-byte native geometry remains the qualified scope.
