# Exact tested binaries

These files are unchanged copies from the package used on the maintainer's console on 25 November 2026. They have not been stripped, patched or rebuilt for publication. Source is in this directory; [TESTING.md](TESTING.md) describes the one same-build handoff and its limits.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| [hen-updater.nro](hen-updater.nro) | 249,912 | `7306bf430548eda53682356c845165726f205caffa9b864e566683cef77a6ae3` |
| [out/bootstrap.bin](out/bootstrap.bin) | 33,600 | `b8cef8818145d333b2ac8d3a31bc97b773a6791b96ef9ba536d91df868820199` |
| [out/ro/main](out/ro/main) | 76,434 | `62a008c3a1cee7c856ed866b74d56e3939cf7cbba00f65ddcb963efced800cd4` |
| [out/hen/main](out/hen/main) | 83,408 | `fe34f0493ee38c3fc13f3837e43711ee64cd3c3a278942094f335d167ee2ff0e` |
| [out/hotupdate-supervisor.nso](out/hotupdate-supervisor.nso) | 91,085 | `dc94b9be620f207a8fd68f89a4b81bafad20ad2632cc5de95ba51b66e15bfd43` |

The bootstrap/RO/HEN/supervisor are a matched set. The ordinary updater is an NRO; these NSO/flat-binary components are separate startup inputs. No generated ExeFS package or system policy accompanies them.

The binaries retain diagnostic paths from public upstream compiler/runtime builds. Those are toolchain strings, not the maintainer's workspace, account or console identifiers. Keeping the exact tested bytes preserves their original checksums.

The project is GPL-2.0-only with separately licensed API/runtime components. See [DEPENDENCIES.md](DEPENDENCIES.md), [LICENSE](LICENSE) and [LICENSES](LICENSES/). The repository Unlicense does not relicense these components.
