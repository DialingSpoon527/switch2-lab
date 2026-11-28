# Runtime binary identities

HEN, RO, supervisor, controller and ping retain the exact bytes used on the
maintainer's console on 25 November 2026, combined manager build **2026112505**.
The bootstrap was rebuilt on November 27 to add optional auto-start exclusions;
a fresh startup and marked-plugin on-demand launch/stop passed on that date. [Hardware observations and offline checks](TESTING.md).

| File in out/ | Role | SD destination | Bytes | SHA-256 |
| --- | --- | --- | ---: | --- |
| `bootstrap.bin` | Native bootstrap | `/kaihen/bin/bootstrap.bin` | 35,264 | `363e99c1c5fb8960e04231f5c672644221c93abb71e77605aa3c20be72bcc313` |
| `ro/main` | Retained RO | `/kaihen/bin/ro/main` | 76,434 | `62a008c3a1cee7c856ed866b74d56e3939cf7cbba00f65ddcb963efced800cd4` |
| `hen/main` | HEN and plugin manager | `/kaihen/bin/hen/main` | 142,629 | `04a96021ca063811d0c5010fb1dd3ffd1bd9f9371d0b40501d91ec6f4a277cd0` |
| `hotupdate-supervisor.nso` | Resident updater supervisor | `/kaihen/plugins/hotupdate-supervisor.nso` | 93,111 | `a2a1c62561b7cc7542cd9aa2d761e7f81e5c852b1b90a90766555a429d85ca2d` |
| `hen-updater.nro` | Optional HEN updater controller | `/switch/hen-updater/hen-updater.nro` | 249,912 | `7306bf430548eda53682356c845165726f205caffa9b864e566683cef77a6ae3` |
| `plugin-starter.nro` | Optional Plugin starter controller | `/switch/p2-plugin-control/p2-plugin-control.nro` | 213,553 | `02cd83619e8b31369350f88e53c934c4cafc08cb14591c4dfaa6ba961b6c3753` |
| `p2ping.nso` | Test ping plugin | `/kaihen/plugins/p2ping.nso` | 26,637 | `037957ddde82ab3c8f8212ba5f9d085199a1539d927385c7bae86eaa4709d19c` |

Bootstrap, RO, HEN and supervisor are a matched startup set. The two NROs are
optional controller apps; they are not substitutes for that runtime. Plugin
starter retains its distinct authored green plug icon and version 0.1.3.
Plugins auto-start unless excluded by the new bootstrap
[controls](docs/PLUGIN-AUTOSTART.md). Ping can be kept for on-demand tests. No generated ExeFS package or signed system policy is included.

The binaries retain public upstream compiler/runtime diagnostic paths. These
are not maintainer workspace or console identifiers. [Origins and runtime
notices](DEPENDENCIES.md), [GPL license](LICENSE), [license collection](LICENSES/).
The repository Unlicense does not override component terms.
