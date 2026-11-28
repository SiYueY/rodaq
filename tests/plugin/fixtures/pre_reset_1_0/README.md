# Pre-Reset 1.0 Real Binary Plugin Fixture

This directory holds a genuine pre-reset 1.0 pure-C RoDAQ plugin compiled directly against
`next @ 6295907ffcc2bd05d8fdc05a5849c5e03a527975` (before Engineering Quality Phase 1 Wave A
Pre-Release 1.0 ABI Reset):

- `rodaq_plugin_descriptor_t` has `sizeof == 56` on `linux-amd64` (no `abi_revision` field).
- `manifest.json` has `plugin.abi == {"major": 1, "minor": 0}` (no `revision` field).
- Zero external shared library dependencies (`ldd`: statically linked).

Used as a negative regression fixture to prove the post-reset Loader deterministically rejects
stale pre-reset 1.0 plugin binaries before `create` / `configure` / `query`.
