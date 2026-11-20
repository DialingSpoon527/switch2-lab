# HEN command bridge

A native NRO and Node.js client that let a PC issue commands after a kaiHEN
session is already running. I made this so I could do more from a terminal
without repeatedly swapping console apps. It has been useful in my own
homebrew workflow. It is largely vibecoded and shared as developer reference,
with no guarantee of function, compatibility, safety, or support.

Version **0.3.0**, wire protocol **2**. [Observed tests and limits](TESTING.md).

## What it does

- Reads HEN status, hosting process information, and syscall capability hints.
- Opens services, clones sessions, manages domain objects, and obtains the
  handles exposed by kaiHEN's homebrew API.
- Sends CMIF IPC requests with inline data, buffers, handles, and objects.
- Streams read-only files to the PC in blocks up to 8 MiB, with SHA-256 and a
  completion record. The SD benchmark copied to disk at 24.12 MiB/s.
- Reads the bridge process's memory and allocates temporary buffers.
- Uploads AArch64 code to a JIT allocation and calls native functions with up
  to eight integer/pointer arguments.
- Runs PC-side scripts or an interactive JavaScript prompt.

The bridge runs in the game process hosting homebrew. It uses the privileges
available to that process and its services. It does not establish a jailbreak
or keep HEN resident after the hosting process is closed.

**There is no authentication or encryption.** It listens on TCP port 8090 on
all interfaces. Any machine that can reach it can issue commands with the
homebrew process's privileges. This is intended for a trusted LAN.

## Build and connect

The included build script is for Windows PowerShell. It expects devkitA64,
devkitPro's `nacptool` and `elf2nro`, and an ounce-devkit installation containing
libounce and libkaihen. Node.js runs the PC client without npm dependencies.
[Dependency references and notices](DEPENDENCIES.md).

From this directory, with paths to your own installations:

```powershell
./build.ps1 -SdkRoot 'PATH_TO_OUNCE_DEVKIT' -DevkitProRoot 'PATH_TO_DEVKITPRO'
```

The output is `build/hen-command-bridge.nro`; `build/manifest.json` records
source, dependency, and output hashes. A fresh build is marked untested until
its own console test. The included [hen-command-bridge.nro](hen-command-bridge.nro)
is the maintainer-tested 0.3.0 build; SDK components are separate.

With HEN already running, copy the NRO to its own directory under `/switch/`
on SD, verify the transfer, and launch it through hbmenu. While it is open:

```powershell
node cli.cjs --host CONSOLE_IP status
node cli.cjs --host CONSOLE_IP script examples/smoke.cjs
node cli.cjs --host CONSOLE_IP dump-sd /switch/example.dat example.dat
node cli.cjs --host CONSOLE_IP repl
```

`HCB_HOST` can supply the host instead. `--port` and `--timeout` override the
defaults. `node cli.cjs help` lists the commands. Press **+** while idle to
return from the bridge.

## Scripting

A script exports an async function receiving `{bridge, h, args, print}`.
The connection and its resources remain available throughout the script.
Use BigInt for addresses and other 64-bit values.

```javascript
module.exports = async ({bridge, print}) => {
  const hen = await bridge.henResource('hen');
  try {
    const reply = await bridge.ipc(hen, {commandId: 0, outSize: 16});
    if (reply.result) throw Error(`GetStatus returned ${reply.result}`);
    print({protocol: reply.data.readUInt32LE(8)});
  } finally {
    await bridge.closeResource(hen);
  }
};
```

`ipc` returns `{result, data, objects, handles, buffers}`. A nonzero service
result is returned explicitly; it does not automatically throw. `h.words`,
`h.qwords`, and `h.hex` build binary inputs. Buffer attributes include
`in|map`, `out|pointer|fixed`, and `out|auto`.

Resource methods include `openService`, `henResource`, `clone`, `domain`,
`asService`, `resources`, and `closeResource`. HEN resource names are `hen`,
`fs`, `settings`, `process`, `sm`, and `loader`; availability depends on HEN.
Close domain children before their parents. Disconnecting releases the
connection's resources, and a new connection starts a new resource session.

Memory and native-call methods include `queryMemory`, `readMemory`, `allocate`,
`writeAllocation`, `jit`, and `call`. Direct memory addresses refer to the
bridge process. Native calls follow the integer/pointer AArch64 calling
convention; floating-point arguments and structure returns need wrappers.
The optional `--native` smoke check runs an integer-addition JIT function.

`dump-sd` opens the input read-only and creates a new PC file and JSON record.
The record marks incomplete copies. From a script, `bridge.dump(resource,
destination, options)` accepts a supported read-only file or recognized
storage object already opened through the bridge. Options include `offset`,
`length`, and `chunk`. Reading a live source sequentially does not produce
an atomic snapshot of data being modified.

## Limits and failure behavior

General packets are limited to 1 MiB, ordinary IPC buffers and memory reads
to 256 KiB, inline IPC data to 128 bytes, and each descriptor category to four
items. Dedicated bulk read replies can carry up to 8 MiB. IPC layouts must
fit the 256-byte TLS area and the service's pointer-buffer capacity. There are
64 resource slots and a 16 MiB budget for owned buffers/JIT allocations.
Known FS and loader roots reject `SetCurrentProcess` to preserve their shared
initialized identity.

Commands run once. A timeout does not cancel an IPC or native call already
executing on the console, and the client does not retry it. An uncertain write
needs inspection before another attempt. Synchronous calls and resource
cleanup can hang if the invoked service stops responding. The exit button
waits for a running synchronous call to return.

Generic IPC and native code can invoke persistent writes through privileged
services, including writes to saves, settings, installed content, and system
storage. A bad command can crash the app, lose the HEN session, corrupt storage,
or break an entrypoint. The protocol's bounds do not make the chosen commands
safe. Compatibility, recovery, long-term reliability, and online-service
restrictions have not been established across other setups.

## Tests and source

```powershell
./test-local.ps1 -SdkRoot 'PATH_TO_OUNCE_DEVKIT' -ZigCompiler 'PATH_TO_ZIG'
```

The host tests compare layouts against the SDK serializer and exercise the
PC client with localhost mock servers. They do not contact the console.
The [test notes](TESTING.md) distinguish those checks from actual console use.

The bridge implementation, client, tests, and scripts were written for this
project with AI assistance. The icon is original SVG artwork. Original
work follows the repository's [Unlicense](../UNLICENSE); dependency licenses
and notices remain separate in [DEPENDENCIES.md](DEPENDENCIES.md).
