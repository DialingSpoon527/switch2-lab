# Observed console use — 25 November 2026

The maintainer used the exact binaries in [BINARY-NOTICES.md](BINARY-NOTICES.md), combined manager revision **2026112505**, on one Switch 2 with the console-only Seedsow launcher and a Mario Kart World host. The source is pinned to official kaiHEN 20261123. Firmware 23.0.1 was recorded during preceding setup; it was not independently reread during this test.

Observed, in order:

1. Fresh combined startup initialized HEN, RO, the updater supervisor and the boot ping plugin. Ping returned 42 for input 41.
2. One on-demand restart stopped the boot ping process and published a new process. Host, HEN and supervisor identities stayed unchanged. Two Loader mounts completed; the manager reported successful restoration, Published and result zero. The replacement ping responded and reported an on-demand restart context.
3. The ping plugin was stopped and independently confirmed absent before updating HEN.
4. The maintainer ran one same-build HEN replacement using the updater NRO and closed the host. A new HEN process initialized; the old HEN was confirmed absent. RO and supervisor identities survived. The deferred transaction and native restart both completed with result/recovery zero, generation 2, exactly one attempt and one success.
5. An accidental second opening of the updater displayed status only. No second update or pending command was found.
6. With the replacement HEN running and the newly opened host still open, a launch request created a fresh ping process. It published, returned 42, and reported an on-demand launch context. Host, replacement HEN and supervisor identities stayed unchanged throughout.
7. Readback verified staged and boot HEN hashes. The generated package contained the exact tested HEN main. Inspection handles were closed.

The NRO and core binaries are unchanged copies, not publication rebuilds. These observations demonstrate one successful sequence. They do not establish repeated reliability, arbitrary plugin compatibility, different-version HEN upgrades, interrupted-update recovery, gameplay stability, other firmware/launcher support or ban safety.

Offline work included native builds, HEN/RO tests, 27 updater transaction/queue/package mock scenarios, paced-launch timing/deadline checks, replacement coordination, controller parsing/I/O/diagnostics, recovery helpers and SDK-encoded lifecycle/delegated-SM tests. The exported test runners target the merged source; mocks are not hardware results.
