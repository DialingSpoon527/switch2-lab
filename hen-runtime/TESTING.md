# Observed console use

The maintainer used the earlier bootstrap `c88fa8212082045ca4b1a2dc74dd049b288b9371d40697b8b2a3f31a412679e3` and the remaining unchanged binaries in [BINARY-NOTICES.md](BINARY-NOTICES.md), combined manager revision **2026112505**, on one Switch 2 with the console-only Seedsow launcher and a Mario Kart World host. The source is pinned to official kaiHEN 20261123. Firmware 23.0.1 was recorded during preceding setup; it was not independently reread during this test.

Observed, in order:

1. Fresh combined startup initialized HEN, RO, the updater supervisor and the boot ping plugin. Ping returned 42 for input 41.
2. One on-demand restart stopped the boot ping process and published a new process. Host, HEN and supervisor identities stayed unchanged. Two Loader mounts completed; the manager reported successful restoration, Published and result zero. The replacement ping responded and reported an on-demand restart context.
3. The ping plugin was stopped and independently confirmed absent before updating HEN.
4. The maintainer ran one same-build HEN replacement using the updater NRO and closed the host. A new HEN process initialized; the old HEN was confirmed absent. RO and supervisor identities survived. The deferred transaction and native restart both completed with result/recovery zero, generation 2, exactly one attempt and one success.
5. An accidental second opening of the updater displayed status only. No second update or pending command was found.
6. With the replacement HEN running and the newly opened host still open, a launch request created a fresh ping process. It published, returned 42, and reported an on-demand launch context. Host, replacement HEN and supervisor identities stayed unchanged throughout.
7. Readback verified staged and boot HEN hashes. The generated package contained the exact tested HEN main. Inspection handles were closed.

Those observations refer to the November 25 startup set; the later bootstrap revision below is separate. The NRO, HEN, RO and supervisor bytes are unchanged. These observations demonstrate one successful sequence. They do not establish repeated reliability, arbitrary plugin compatibility, different-version HEN upgrades, interrupted-update recovery, gameplay stability, other firmware/launcher support or ban safety.

Offline work included native builds, HEN/RO tests, 27 updater transaction/queue/package mock scenarios, paced-launch timing/deadline checks, replacement coordination, controller parsing/I/O/diagnostics, recovery helpers and SDK-encoded lifecycle/delegated-SM tests. The exported test runners in `plugin-tools/` target the merged source; mocks are not hardware results.

## November 27 bootstrap opt-outs

The bootstrap now keeps default auto-start for unlisted plugins, with an optional
blacklist and per-plugin markers for on-demand-only installs. It rebuilt to
35,264 bytes, SHA-256 `363e99c1c5fb8960e04231f5c672644221c93abb71e77605aa3c20be72bcc313`.
HEN, RO and supervisor rebuilt byte-for-byte identically to the tested set.

The native build, HEN/RO checks, updater transaction/release checks and plugin
coordination/runtime checks passed. Parser checks cover CRLF/BOM/comments,
empty configuration, duplicates, name/size/count boundaries, all-or-nothing
rejection, unlisted-plugin compatibility and 20,000 random byte inputs under
ASan/UBSan. These checks do not emulate kernel IPC, SD errors or startup timing.

The maintainer booted the new bootstrap through the console-only Seedsow
launcher, reached a working homebrew session and opened the command bridge.
Read-only inspection verified the exact installed bootstrap and opt-out marker
hashes, HEN initialization result zero and game-arming result zero. The saved
startup log reached READY. The unmarked updater supervisor was the only running
managed boot plugin. All nine marked plugins were recorded as skipped with
result zero and PID zero. No shared blacklist was installed, so the absent-file
default path was exercised as well.

While its marker remained present, the ping plugin then launched on demand,
published a fresh process and returned 42 for input 41. It was stopped once;
the manager confirmed process exit. HEN, supervisor and the bridge's host stayed
unchanged. All bridge inspection resources were closed. The marker stays present
and the ping NSO remains available for a later explicit launch.

This is one fresh startup and one marked-plugin launch/stop sequence on one
console. Nonempty blacklists, malformed configuration and marker lookup errors
were not exercised on hardware; the parser's offline checks are separate.
This test did not repeat HEN replacement or test arbitrary third-party plugins,
ordinary gameplay stability, other firmware or recovery from interrupted writes.
Firmware was not independently reread during this test.
