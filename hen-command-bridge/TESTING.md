# Observed use of 0.3.0

The maintainer launched the exact NRO through hbmenu on November 19, 2026,
after starting kaiHEN. The bridge reported HEN protocol 7 and initialization
result 0. The reported console firmware was 23.0.1. The PC used Windows and
Node.js 24.13.1. devkitA64 GCC 16.1.0 built the NRO.

| Test on one console and LAN | MiB/s |
| --- | ---: |
| Generic IPC reads of a synthetic 64 MiB SD file | 11.84 |
| Bulk reads of the same SD file | 24.96 |
| Bulk copy of the same file to PC with SHA-256 | 24.12 |
| Network-only transfer of a zeroed buffer | 36.28 |

Checksums matched for the PC copy and for a separate readback of the NRO.
The SD benchmark file was removed after the test. Later use copied 146 game
save entries across 80 application IDs to a PC using read-only save handles
and bulk file reads. The 6,577 copied files were checked against the received
bytes using SHA-256. No restore was attempted.

Rates describe the measured setup. Installed-content IStorage reads were
not tested on hardware. Other firmware, HEN versions, hosts, concurrent
clients, recovery behavior, and long-running use were not evaluated. Generic
IPC and native calls retain the ability to modify persistent storage.

## Offline checks

The C test checked 5,664 accepted and 386 oversized IPC layouts against the
SDK serializer and rejected 13,950 invalid layouts. It also checked file
interface and range restrictions. Twelve Node.js tests passed, including
fragmented bulk replies, checksum handling, and partial-copy behavior.

## Tested source identity

| File | SHA-256 |
| --- | --- |
| `main.c` | `1b0dbabfbf8f7b9e29f037f4bafae46b7a6b2939e98440a0dce69b934fb79081` |
| `protocol.h` | `73aa083112a8eb0059cdb833335cea32cba67d63092a1a2e550f8b059b7365e1` |
| `client.cjs` | `28e8c6e0b92fc0af31b842ce327bad0164080d7de8218acb507019749787a557` |
| `cli.cjs` | `21f8597f5809ea38ad091d99837165b90b27b5d1d1b66f3162c7455a38af85a9` |
| `dump.cjs` | `32129872c61db723b5e1cbea2a307f08db2b3addca6ea6dd3ba36e1504b34d25` |

The included [NRO](hen-command-bridge.nro) is the same 419,476-byte file used
on the console. Its SHA-256 is `109d1cac781125f77732ad104f3214949caf7dbc5ef76e7f8e22aab7b6239102`. See the
[third-party notices](NRO-NOTICES.md). A fresh build has its own binary hash
and needs its own hardware test.
