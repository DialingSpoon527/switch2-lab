# Changes in this HEN-only revision

This revision retains the V11-derived HEN process handoff while removing live RO replacement. Existing RO routes the handoff; it is not itself replaced. It is separate from V24/V24.1's experimental RO-backend updater.

The supplied HEN-only source split was integrated into a complete official kaiHEN 20261123 source tree, retaining the newer RO shared-library mapping and cleanup changes. The named source merge completed without text conflicts.

Changes include:

- Deferred protocol 5 and a digest/size-bound manifest queue request, closing the gap between the controller's preflight and the supervisor's selected file.
- Hashing the exact bytes copied into the candidate package before Loader interception.
- Treating an uncertain old-HEN stop reply as past the rollback boundary.
- Stopping speculative recovery after uncertain Loader/PM, RO-stage, RO-rollback or candidate-termination outcomes.
- Avoiding later checkpoint writes after an unresolved transaction.
- Strict command/manifest parsing, a protocol mismatch check, a readable NRO result screen and accurate log-save error reporting.
- Header dependencies/build ordering and a PC build/package/three-way-port helper.

The original byte-for-byte V11 function hash no longer applies because its error paths changed. Fault-injection fixtures execute the changed function bodies with mocked services instead.

One same-build live handoff passed on the maintainer's console. [Observed results and remaining limits](TESTING.md) are separate from host tests and source analysis. No FS memory-capture extension is included in this source or these binaries.

## Combined plugin manager — revision 2026112505

The matching HEN now supports launch/stop/restart with managed lifecycle state, cached diagnostics and digest-bound plugin selection. Paced Loader polling allows a stable running host to remain open for plugin launch. The startup lookup uses the existing delegated per-plugin SM proxy, with no fresh identity registration. The proxy still shares its initialized upstream master: a plugin that never registers can stall other lookups even after quarantine.

HEN replacement is coordinated with plugin-manager work. It rejects active, pending, quarantined and shutdown/restart work and live non-supervisor plugins. The supervisor is protected from generic stop/restart and forwards its existing shell capability to replacement HEN. Private command IDs are distinct. Ordinary HEN replacement still waits for the actual host to exit. [Current observations](TESTING.md) include a fresh plugin process launched through replacement HEN, not just a boot-plugin restart.
