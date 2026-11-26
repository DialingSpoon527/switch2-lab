# Plugin starter

**For developers inspecting kaiHEN. Practically all of this is vibecoded and AI-written, with human direction and testing. No guarantee of function, safety, compatibility or support.**

This adds on-demand background-plugin launch, stop and restart to the matching kaiHEN build. The small hbmenu NRO is a controller and diagnostics screen; HEN owns the background processes. A CLI client can issue the same public IPC commands without opening the NRO.

On my console, a ping plugin restarted while the host stayed open. I then stopped it, replaced HEN through the updater, and launched a **fresh plugin process through the replacement HEN**, again with the host still open. [Observed results](TESTING.md).

## Matching components

Use the combined manager build **2026112505**, HEN API protocol 7 and updater deferred protocol 5. The shared HEN/bootstrap/RO/supervisor source and tested binaries live in [hen-updater](../hen-updater/). The NRO alone does not add these features to stock HEN. Older standalone plugin-manager and updater components are not interchangeable with this set.

| Included file | Tested SD destination |
| --- | --- |
| [plugin-starter.nro](plugin-starter.nro) | `/switch/p2-plugin-control/p2-plugin-control.nro` |
| [p2ping.nso](p2ping.nso) | `/kaihen/plugins/p2ping.nso` |

The NRO retains the tested name **Plugin starter**, version **0.1.3**, and its distinct authored green plug icon. The ping NSO is a test service, not a useful background app by itself. Keeping it in `/kaihen/plugins/` also makes it a startup plugin on the tested configuration. Do not copy arbitrary plugins there expecting them to remain inactive.

This is an informal working example, not an automatic setup package. [The combined component table](../hen-updater/README.md#included-tested-files) identifies the startup inputs and separate companion dependencies. No Seedsow save, firmware, system policy or SDK is included.

## Controller commands

With the matching HEN already running, opening the NRO with **no command file** displays cached diagnostics. It does not launch, stop or restart anything. The last report is written to SD at `/kaihen/plugin-manager/result.txt`.

For an operation, put exactly one of these lines in `/kaihen/plugin-manager/command.txt`, then open Plugin starter:

| Line | Action |
| --- | --- |
| `diag` | Cached manager diagnostics |
| `list` | Managed process listing |
| `launch p2ping` | Start the installed ping plugin when stopped |
| `stop p2ping` | Stop that managed plugin |
| `restart p2ping` | Stop and relaunch that managed plugin |
| `ping p2ping` | Exercise the demo's ping command |
| `context p2ping` | Read the demo's lifecycle context |
| `cancel` | Cancel pending launch work where the manager permits it |

The command is consumed before dispatch. Launch/restart acceptance is asynchronous: leave the controller with **+** and reopen it without a new command to inspect the result. **The host game can stay open** for a stable plugin launch or restart. Acceptance alone does not mean a plugin started; look for a published process and result zero. Inspect the saved report after a timeout rather than blindly repeating an operation.

Launch/stop/restart also accept another safe plugin name, without `.nso`; the file must be `/kaihen/plugins/NAME.nso`. Ping/context are specific to this demo's ABI. A digest of the installed NSO is supplied with launch requests. That binds the selected bytes; it is not a signature, trust check or sandbox.

The supervisor is protected from generic plugin stop/restart. **HEN replacement is different:** stop other managed plugins first, queue the updater request, and close the actual host game. Plugin registrations are not migrated across HEN generations. After replacement, compatible plugins can be launched again.

## Build and inspect

Use Linux or WSL with GNU Make, Python 3, G++ and the Linux devkitA64/libounce SDK described in [dependencies](../hen-updater/DEPENDENCIES.md). From this directory:

```sh
export DEVKITPRO=/opt/devkitpro
export LIBOUNCE="$DEVKITPRO/libounce"
sh test.sh
sh build.sh
```

`build.sh` builds the shared HEN tree and the two examples. New outputs stay under `examples/`; the included tested downloads are not overwritten. An individual example can be built with `make -C examples/plugin-control KAIHEN_SOURCE=/absolute/path/to/hen-updater`. The inherited HEN release helper packages core HEN/updater components; it does not package this separate example directory.

The public IPC API is in [protocol.h](../hen-updater/libkaihen/include/kaihen/protocol.h), [diagnostics.h](../hen-updater/libkaihen/include/kaihen/diagnostics.h) and [lifecycle.h](../hen-updater/libkaihen/include/kaihen/lifecycle.h). The example sources show launch requests and lifecycle callbacks. The host tests use mocks and actual SDK encoders; they do not emulate the console kernel or prove hardware recovery.

## Limits

Only the ping example and one integration sequence were tested on one console. Other plugins, repeated updates, different HEN versions and ordinary gameplay stability remain untested. A bad or unregistered plugin can stall shared service lookups even after the manager times out; recovery may require rebooting and using Seedsow again. App crashes, loss of resident HEN and uncertain FS state are possible. SD backups do not guarantee recovery from persistent-storage damage, and there is no online-service safety guarantee.

The shared HEN is a GPL-2.0-only derivative, its client/runtime API is ISC, and the examples retain their MIT license markers. [Source origins and notices](LICENSES.md). The repository Unlicense does not override these terms.
