# HEN-only updater

**For developers inspecting kaiHEN. This is largely vibecoded, AI-written work, with human direction and testing.** It is an informal working example, with no guarantee of function, compatibility, safety, packaging or support.

I made this to replace the HEN process without rebooting and repeating Seedsow's 4/4 for each compatible HEN change. It keeps the existing RO and supervisor running. It does not make an arbitrary future kaiHEN release compatible with an older running environment.

The current combined build **2026112505** also supports [Plugin starter](../plugin-starter/). On my console, initial startup, a plugin restart with the host open, **one same-build live HEN replacement**, and a fresh plugin launch through the replacement HEN all worked. RO and the supervisor survived the HEN replacement. [Exact observations and limits](TESTING.md).

Based on official kaiHEN **20261123**, commit `3b23b5dc6e75539855eca69ffdda371ce69631bb`, with the V11-derived HEN handoff and HEN-only split. The source and original notices are retained. This project is a **GPL-2.0-only derivative**, with separately licensed components; the repository's Unlicense does not override those terms. [Origins and dependencies](DEPENDENCIES.md).

## What changes while running

| Component | Live update behavior |
| --- | --- |
| HEN | Replaced after the actual host game exits and idle checks pass |
| RO | Remains running and routes sessions to the replacement HEN |
| Supervisor | Remains running and checks the handoff |
| Bootstrap, hbloader, hbmenu, libraries | Need their own matching files; live HEN replacement does not upgrade them |
| Seedsow save | No save edit is part of this updater |

This combines the plugin manager and HEN-only updater; use its matched bootstrap, HEN and supervisor together. It supersedes the earlier updater-only binary set in this directory. This is separate from the experimental V24/V24.1 RO-backend updater. Their bootstrap and supervisor are not interchangeable. The privileged filename **hotupdate-supervisor.nso** is part of the bootstrap handoff and cannot simply be renamed.

## Included tested files

| File | SD destination for the tested untethered layout |
| --- | --- |
| [hen-updater.nro](hen-updater.nro) | `/switch/hen-updater/hen-updater.nro` |
| [out/bootstrap.bin](out/bootstrap.bin) | `/kaihen/bin/bootstrap.bin` |
| [out/ro/main](out/ro/main) | `/kaihen/bin/ro/main` |
| [out/hen/main](out/hen/main) | `/kaihen/bin/hen/main` |
| [out/hotupdate-supervisor.nso](out/hotupdate-supervisor.nso) | `/kaihen/plugins/hotupdate-supervisor.nso` |

These are the exact tested binaries, not rebuilt copies. [Binary hashes and runtime notices](BINARY-NOTICES.md). The NRO alone cannot add updating to an already-running stock HEN. Matching bootstrap, RO, HEN and supervisor must start together in a fresh session. Other untethered launchers may use different paths.

The current official acquisition package instead keeps bootstrap and RO outside its `sdcard/` directory. The build helper follows that official layout. The table above is the separately checked untethered SD layout.

The matching official **20261123** hbloader, hbmenu and `/kaihen/lib` files are separate dependencies. This directory does not bundle those companion apps, firmware, system policies, save data or SDKs. The build helper can use the pinned [official release ZIP](https://github.com/ouncebrew/kaihen/releases/tag/20261123) for companion files and their licenses. Dynamic homebrew support needs the updated RO/hbloader and libraries; changing only HEN cannot add it to an older environment.

## Build from source

Requirements: Linux, or an installed WSL distribution on Windows; Python 3.10+; GNU Make; host G++ with ASan/UBSan; and a Linux Switch 2 SDK containing devkitA64, devkitPro tools and libounce 20261123 or newer. Git is needed for porting to another upstream revision.

1. Clone or download this repository and open this directory.
2. Windows: double-click **Build-Release.cmd**. Linux: run `sh build-release.sh`.
3. Choose **1. Build this source**.
4. Enter the Linux SDK folder, or press Enter to detect it. The Windows `.exe` SDK is not a Linux toolchain. The Windows helper selects an ordinary WSL distribution rather than Docker's minimal environment.
5. Select the matching official binary-release ZIP. For this pinned version, Enter downloads the known 20261123 release and verifies its checksum.
6. Choose a new output folder. Existing folders are not overwritten.

The helper runs tests, builds the components in a fresh temporary source tree, and produces separate **first-install**, **hen-update** and **source** ZIPs, checksums and a build record. It does not contact a console or install anything to SD. Build metadata says `hardware_tested: false` because a new build has not inherited the included binaries' hardware result.

```sh
python3 tools/release.py build --devkitpro /opt/devkitpro --official-zip /path/kaiHEN-20261123.zip --out /path/new-output
```

## Request a HEN-only update

This requires the matching supervisor already running. Preserve a working entrypoint and a usable SD restoration path before experimenting. The included binary set's initial setup is not an automatic installer.

1. Stop other managed plugins first and confirm they have exited. The supervisor is retained; generic plugin controls cannot stop it. Then extract the helper's **hen-update.zip** on the PC.
2. Copy its `sdcard/` contents to SD. This places the candidate at `/kaihen/hotupdate/staged/hen/main` and its checksum manifest at `/kaihen/hotupdate/hen-package.txt`.
3. Finish all transfers and leave staged files unchanged while a request is pending.
4. For a dry run, copy `commands/DRY-RUN.txt` to SD as `/kaihen/hotupdate/hen-command.txt`. Open HEN updater. After it says **QUEUED HEN DRY RUN**, press **+**, then **HOME â†’ X â†’ Close** on the host game. Leaving the NRO alone does not close the host process.
5. For a live handoff, use `commands/APPLY.txt` at that same command path. Open the NRO and check for **QUEUED HEN-ONLY REPLACEMENT**, then leave and close the host game as above.
6. Allow the handoff to finish before launching another game. The runtime result and `/kaihen/hotupdate/transaction-TICKET-hen.txt` distinguish a completed handoff from a queued request. A successful ticket ends with `phase=END result=00000000` and the old HEN marked stopped.

The manifest is exactly `target=hen` and `sha256=64_HEX_DIGITS`, on separate lines. The command is consumed before validation or queue IPC. Copying the helper's SD layout alone does not arm an update: command examples sit outside `sdcard/`.

`STATUS` or an absent command shows status. `CANCEL_DEFERRED_V2` cancels a pending request before execution, not an active transaction. The helper also provides `DEFER_DRYRUN_V1` and `APPLY_HEN_PACKAGE_V1` examples. Deferred protocol **5** is required; mismatched supervisors are refused.

The supervisor waits up to three minutes for host exit and two idle observations. Active or quarantined plugin-manager work, and live non-supervisor managed plugins, block replacement. Plugin registrations are not migrated across HEN generations; launch compatible plugins again after replacement. At most three live attempts are permitted per supervisor boot. Only success plus a responsive replacement clears the per-attempt latch. `Unresolved` stops further attempts; a missing END checkpoint can be evidence of a stopped transaction. Do not treat a timeout as permission to repeat it blindly.

Live replacement leaves `/kaihen/bin/hen/main` unchanged. Using a different HEN after a future reboot is a separate SD change. This handoff is not persistence across shutdowns.

## Start from a newer upstream release

The same helper can port updater changes onto another kaiHEN source ZIP or folder. Choose **2** in the wizard, enter the source tag/commit and a new output folder. It uses the pristine GPL upstream baseline in `tools/upstream-base.zip` for a three-way merge.

`PORT-REVIEW.json` lists merge conflicts. Text files keep conflict markers; other conflicts preserve available alternatives. Upstream changes to HEN/plugin ABI, RO handoff or bootstrap geometry can require code changes even when text merges cleanly. Matching companion files are still required.

```sh
python3 tools/release.py rebase --upstream /path/new-source.zip --revision TAG-OR-COMMIT --out /path/new-source-with-updater
python3 tools/release.py build --reviewed --official-zip /path/matching-release.zip --out /path/new-output
```

The second command runs from the prepared source directory after reviewing the port. A clean build or merge is not a hardware compatibility result. The helper does not commit or publish anything.

## Limits

The successful console test replaced HEN with another instance of the **same binary**. Different-version upgrades, repeat handoffs, rollback failures, interrupted transactions and ordinary gameplay stability remain untested.

The transaction temporarily uses privileged Loader/FS state. A failure can crash an app, lose the resident HEN session, or leave uncertain FS state affecting later storage operations. Restarting requires the working entrypoint again; an SD backup is not a guarantee of console recovery. Direct output writes reviewed here target SD, but that does not establish every consequence of invoked services. There is no ban-safety guarantee.

SHA-256 checks bind the queued file and the copied package bytes; they are not signatures or a sandbox. Synchronous service calls can still block. This is source to examine and adapt within its licenses, not a supported release.
