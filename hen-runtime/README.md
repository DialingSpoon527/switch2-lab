# HEN runtime

**For developers inspecting kaiHEN. Practically all of the added code is vibecoded and AI-written, with human direction and testing. No guarantee of function, safety, compatibility or support.**

This is one modified kaiHEN build with two related features: replacing the running HEN process, and starting or restarting managed background plugins. They share HEN, bootstrap and supervisor changes, so the matching source and binaries live together here.

I made these changes to avoid repeating Seedsow's 4/4 for every compatible HEN change, and to start useful background services during an existing session. The hbmenu apps are optional controls for those features. The runtime owns the processes; a PC client can issue the same IPC commands.

On my console, this exact build completed startup, restarted a ping plugin while the host stayed open, replaced HEN with another instance of the same binary, then launched a **fresh plugin process through the replacement HEN**. RO and the supervisor stayed running. [Observed tests and limits](TESTING.md).

## What is here

| Part | Role |
| --- | --- |
| `bootstrap/`, `hen/`, `ro/`, `common/` | Matching native runtime source |
| `hotupdate-supervisor/` | Resident supervisor for the HEN handoff |
| `hotupdate-controller/` | Optional HEN updater app source |
| `libkaihen/` | Client/plugin API and lifecycle runtime |
| `plugin-tools/` | Optional Plugin starter app, ping example and focused tests |
| `out/` | Exact console-tested binaries, described in [BINARY-NOTICES.md](BINARY-NOTICES.md) |
| `tools/` | Core build/package and source-porting helpers |

Based on official kaiHEN **20261123**, commit `3b23b5dc6e75539855eca69ffdda371ce69631bb`. The tested combined manager build is **2026112505**, with HEN protocol 7 and updater deferred protocol 5. These are a matched set; adding a controller NRO to stock HEN does not add the runtime features.

## The two operations

**Plugin management:** launch, stop, restart and inspect compatible managed plugins. A stable host can remain open during a plugin launch or restart. Requests are asynchronous; acceptance is not proof of a published process. [Plugin management and controller commands](docs/PLUGIN-MANAGEMENT.md).

**HEN replacement:** stop other managed plugins, queue a compatible HEN candidate, and close the actual host game. The supervisor replaces HEN while retaining RO and itself. Plugin registrations are not migrated; compatible plugins can be launched again afterward. [HEN update workflow and build helper](docs/HEN-UPDATING.md).

Opening either controller without its one-shot command file displays status. Neither controller is required if another client sends the equivalent IPC requests. They remain separate small apps for now; this directory is the single project containing their shared runtime.

## Build and inspect

Use Linux or WSL, Python 3.10+, GNU Make, host G++ and a Linux devkitA64/libounce SDK. SDKs and official companion apps are separate dependencies. [Origins, dependencies and licenses](DEPENDENCIES.md).

From this directory:

```sh
export DEVKITPRO=/opt/devkitpro
export LIBOUNCE="$DEVKITPRO/libounce"
make -j2
sh plugin-tools/test.sh
sh plugin-tools/build.sh
```

The root Makefile builds the core, supervisor and updater controller. `plugin-tools/build.sh` also builds the Plugin starter controller and ping example. New outputs stay in component build directories; they do not overwrite the tested files in `out/`.

`Build-Release.cmd` and `build-release.sh` run the inherited core build/package helper. It produces fresh first-install, HEN-update and source packages; it does not package the separate Plugin starter/ping example binaries. New builds remain untested until exercised on hardware. The helper's source-porting mode can require manual reconciliation even after a clean merge.

## Limits

The successful HEN handoff used the **same binary** on both sides. Different-version upgrades, repeated replacements, arbitrary plugins, interrupted transactions and normal gameplay stability remain untested.

A plugin that never registers can stall shared service lookups even after the manager times out. A failed privileged operation can crash an app, lose resident HEN or leave uncertain FS state. Recovery may require rebooting and using the working Seedsow entrypoint again. SD file backups do not guarantee recovery from persistent-storage damage. No ban-safety guarantee is established.

The runtime is a **GPL-2.0-only derivative**, with ISC API/runtime components and MIT-marked examples. The repository Unlicense does not override their terms. Original licenses, notices and provenance are retained in [LICENSE](LICENSE), [LICENSES](LICENSES/) and [DEPENDENCIES.md](DEPENDENCIES.md).
