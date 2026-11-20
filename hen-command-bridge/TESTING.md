# Observed use of 0.2.1

These are observations from one console and one PC on November 18, 2026.
They are not compatibility, safety, or performance guarantees.

The maintainer confirmed launching version 0.2.1 through hbmenu after a completed
kaiHEN launch. The bridge reported HEN protocol 7 and initialization result 0.
Console firmware was reported as 23.0.1; the loader's `runtimeHosVersion` field
is not independent firmware verification. The PC used Windows and Node.js
v24.13.1. The compiler was devkitA64 GCC 16.1.0.

## Console observations

- HEN status through generic IPC succeeded.
- A temporary allocation accepted test bytes and returned matching bytes.
- A JIT function adding two integers returned 42.
- Read-only SD hash verification succeeded for 274 files totaling 4,274,428
  bytes, taking 2.65 seconds. Their upload had used the earlier 0.2.0 build;
  that upload is not evidence of write behavior in 0.2.1.
- Two old bridge NROs were removed from their exact SD directories after their
  contents matched locally retained builds. Only empty directories were then
  removed. The current build's SD file matched its original hash afterward.
- The smoke checks and file-verification connections ended with zero remaining
  resources reported by the bridge.

For 40 small status requests, median latency changed from 61.98 ms on version
0.2.0 to 1.19 ms on 0.2.1. Reading 1 MiB from an owned RAM buffer in 64 KiB
chunks changed from 0.757 to 15.64 MiB/s. These are short measurements on the
same LAN, with no network isolation or long-duration stress test. The RAM result
is not a filesystem write-throughput measurement.

## Offline observations

The C layout test checked 5,664 accepted layouts against the installed SDK
serializer, rejected 386 oversized layouts, and rejected 13,950 invalid domain
layouts. All seven Node.js client tests passed, including fragmented replies,
timeouts without retries, request IDs, IPC results, and 64-bit addresses.

The build/test wrappers accept external SDK and toolchain paths. On the test
PC, the Windows wrapper built an NRO successfully and the test wrapper passed
the same layout checks and all seven Node.js tests. No console was contacted
by those build/test steps.
A cross build does not establish that its new output binary was tested on the
console, even when its C sources match a previously tested build.

## Tested source identity

The published runtime and client files are byte-identical to the tested 0.2.1
sources. Their SHA-256 hashes are:

| File | SHA-256 |
| --- | --- |
| `main.c` | `995a71ede5b1cb642dd212e1ea7805b1d87233bd599c464144eb45ad1b529e27` |
| `protocol.h` | `50cf389f643a5b05978ce27145512543f63dac9ab02a93433ad5027a599bc57a` |
| `client.cjs` | `0ec5599f0f5a0c3f1c975e899992b196ad3524cee956a05286bb69d88053a72d` |
| `cli.cjs` | `42e41bc359713795770340fb9b6a43c100ff9180ad877068acb3e277c0ad1b82` |

The original locally tested NRO had SHA-256
`a7a9d2f0fe9ef5183621fbc2e8297fa8ac0a09d945aa8fb9205fc2c9d489cfb2`.
That exact build is included as [hen-command-bridge.nro](hen-command-bridge.nro). Debug paths, build location, and toolchain changes
can make another build differ; matching version text alone is insufficient.

## Limits of the observations

One console, firmware report, HEN protocol, and LAN were exercised. Other
firmware, HEN versions, hosts, concurrent clients, recovery paths, and long-term
behavior were not established. The tests did not establish ban safety or safe
use of arbitrary IPC/native commands. Invalid service operations can still
cause persistent damage despite passing the protocol checks.
