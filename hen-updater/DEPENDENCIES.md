# Source origins, dependencies and licenses

This is a modified **GPL-2.0-only kaiHEN derivative**, not a project-wide Unlicense grant. [LICENSE](LICENSE) is the upstream GPL text. ISC components and linked runtime code retain their own terms.

| Input | Revision / use |
| --- | --- |
| [ouncebrew/kaihen](https://github.com/ouncebrew/kaihen) | Official 20261123, commit `3b23b5dc6e75539855eca69ffdda371ce69631bb`; full matching source retained |
| V11 source snapshot and HEN-only split | Earlier local updater changes integrated against official 20261117.1 commit `9aa371668e95e29c185bf6d31293aa13a0503a60`, then merged onto 20261123; changes summarized in REVIEW.md |
| [libounce](https://github.com/ouncebrew/libounce) | Package 20261123-1; open-source homebrew API/runtime linked into the binaries; separate SDK |
| [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) / devkitA64 | Linux SDK, GCC 16.1.0, ELF/NSO/NRO/NACP tools; not bundled |
| libkaihen | Included kaiHEN client/plugin API sources under ISC; notice in `libkaihen/LICENSE` and `LICENSES/libkaihen-ISC.txt` |
| GNU Make, Python 3.10+, G++ | Build helper and host tests; Git provides the three-way source merge |
| [Official companion release](https://github.com/ouncebrew/kaihen/releases/tag/20261123) | Matching hbloader/hbmenu/shared libraries and companion licenses; downloaded or supplied separately |

Input SHA-256 values:

- Official source ZIP / bundled pristine merge baseline: `44fc7c3ee3ccb5725c18058f851bf1b3bd127c3f038e2a31618238ba6cf94d4b`.
- Official companion ZIP: `24c0e68125817ab751f96985af63347a4e8bec7aced37f713b3fbb3d0eb4680c`.
- libounce 20261123-1 package: `16cb51c8c83a461fec8e88a906e8ac3d0753e3924cdc82668ef861624c0252dc`.
- Supplied HEN-only source split: `c308957a14a8aea25c1e3f59a45d0e0d091698dd3c0f8615c50b2a279b433097`.

The original [upstream README](UPSTREAM-README.md), source license markers, API license, release-license collection and pristine source archive retain their original origin and terms. No original authorship is claimed for upstream code. Changes authored for this updater are offered under the corresponding project/file licenses; this cannot waive third-party rights.

The included binaries contain linked runtime code. Notices are in [LICENSES](LICENSES/): libounce ISC, libnx ISC, libkaihen ISC, mimalloc MIT, newlib's copyright/license collection, and GCC's runtime exception plus GPLv3 text. The newlib notice originates from [its official collection](https://sourceware.org/newlib/COPYING.NEWLIB); GCC notices originate from [GCC source](https://github.com/gcc-mirror/gcc/blob/master/COPYING.RUNTIME). Keep the applicable notices with redistributed binaries.

No Nintendo firmware/game/save content, signed system policy, proprietary SDK, private key or capture is included. The running console supplies any signed policy used to construct a launch package; generated packages are not bundled here. SDK dependencies are obtained separately from their legitimate upstream projects.
