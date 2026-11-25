# Observed tests and limits

## Console use — 25 November 2026

The maintainer used the exact binaries listed in [BINARY-NOTICES.md](BINARY-NOTICES.md) on one Switch 2 with the console-only Seedsow launcher and a Mario Kart World host. The source is based on kaiHEN 20261123; HEN reports API protocol 7 and the updater uses deferred protocol 5. Firmware 23.0.1 was recorded during the preceding setup; it was not independently reread during this handoff test.

Observed:

1. A fresh Seedsow startup completed, with zero reported bootstrap, Loader-restoration and channel-restoration errors. R-held hbmenu and command bridge opened.
2. The updater NRO consumed a dry-run command. The supervisor observed the host process exit and reached DryRunPassed with result 0. HEN stayed running; no live attempt was counted.
3. The maintainer ran manifest apply through the NRO, closed the host game and later reopened the bridge.
4. The supervisor reported HenReplaced, native RunningReplacement, result/recovery 0 and generation 2. The previous HEN process no longer existed; a different HEN process was live and initialized successfully.
5. RO and the supervisor retained their original process identities. An independent RO identity query agreed. The durable completion checkpoint ended successfully and the one-shot command was absent.
6. The replacement passed the supervisor's health check. Counters showed one attempt and one success, with another request permitted. A second replacement was not executed.
7. The bridge worked through the replacement. Independent readback verified the staged HEN, the main inside the generated package and the unchanged boot HEN file against the tested hash. Inspection handles were closed.

This demonstrates **one same-build process handoff**, not an upgrade between different upstream versions. It does not establish repeatability, gameplay stability, failure recovery, compatibility with other firmware/launchers, persistence across reboot, or online-service safety.

## PC and offline checks

- Complete AArch64 build of libkaihen, RO, HEN, bootstrap, supervisor and controller with devkitA64 GCC 16.1.0 and libounce 20261123.
- Upstream HEN mount/SM request tests and RO wire/image/session/mapping tests under host sanitizers.
- Strict request/manifest parser and HEN-only path checks.
- Actual transaction function bodies with mocks: 13 handoff scenarios, eight digest-bound queue scenarios and six package byte-binding/I/O scenarios.
- Eight build-helper regressions, including merge conflicts, preservation of upstream changes, invalid ZIP paths and refusal to overwrite outputs.
- Guided Windows-to-Ubuntu build with Windows paths, clean source-port exercise, package readback, NSO segment hashes and NRO metadata.
- Repeated fresh builds produced identical tested binaries.

Mocks do not establish kernel/service behavior, memory-map permissions, timing, handle lifetimes or hardware safety. The build helper itself was used to produce the tested package; future rebuilt outputs remain distinct revisions.
