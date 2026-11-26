# Exact tested combined-build binaries

These are unchanged copies used on the maintainer's console on 25 November 2026, manager build **2026112505**. No publication rebuild or binary patch was made. [Hardware results](TESTING.md).

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| [hen-updater.nro](hen-updater.nro) | 249,912 | `7306bf430548eda53682356c845165726f205caffa9b864e566683cef77a6ae3` |
| [out/bootstrap.bin](out/bootstrap.bin) | 33,984 | `c88fa8212082045ca4b1a2dc74dd049b288b9371d40697b8b2a3f31a412679e3` |
| [out/ro/main](out/ro/main) | 76,434 | `62a008c3a1cee7c856ed866b74d56e3939cf7cbba00f65ddcb963efced800cd4` |
| [out/hen/main](out/hen/main) | 142,629 | `04a96021ca063811d0c5010fb1dd3ffd1bd9f9371d0b40501d91ec6f4a277cd0` |
| [out/hotupdate-supervisor.nso](out/hotupdate-supervisor.nso) | 93,111 | `a2a1c62561b7cc7542cd9aa2d761e7f81e5c852b1b90a90766555a429d85ca2d` |

Bootstrap, RO, HEN and supervisor are a matched startup set. The companion [Plugin starter NRO and ping example](../plugin-starter/BINARY-NOTICES.md) use this set. No generated ExeFS package or signed policy is bundled.

Public upstream compiler/runtime diagnostic paths are retained in exact tested bytes. [Origins and runtime notices](DEPENDENCIES.md), [GPL license](LICENSE), [license collection](LICENSES/). The repository Unlicense does not override component terms.
