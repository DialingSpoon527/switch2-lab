# Combined-build hardware results

The binaries in this directory now belong to combined manager revision **2026112505**, based on kaiHEN 20261123. On 25 November 2026 the maintainer used this exact set for startup, a host-open ping restart, one same-build HEN replacement through the updater NRO, and a fresh plugin launch through replacement HEN with the host open.

The old HEN exited, replacement HEN initialized, supervisor and RO survived, and the durable completion checkpoint and counters showed one successful attempt. A second opening of the updater displayed status only. Staged HEN, boot HEN and the main inside the generated package matched the tested hash.

[Detailed observations and limits](../plugin-starter/TESTING.md) distinguish the current hardware sequence from offline checks. The earlier updater-only r3 build also passed its own dry run and one same-build replacement; that earlier result is not used as proof of this combined revision. Firmware 23.0.1 was previously recorded, not independently reread during the current test.

Different-version upgrades, repeated replacements, interrupted transactions, arbitrary plugins and ordinary gameplay stability remain untested. The current shared lookup path can still stall on an unregistered plugin. No ban-safety or recovery guarantee is established.
