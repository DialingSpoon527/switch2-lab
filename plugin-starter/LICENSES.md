# Origins and licenses

The combined runtime is a modified [ouncebrew/kaihen](https://github.com/ouncebrew/kaihen) **20261123**, commit `3b23b5dc6e75539855eca69ffdda371ce69631bb`. Its complete matching source is in [hen-updater](../hen-updater/), under the upstream GPL-2.0-only terms with separately licensed API/runtime components. [Dependencies and retained notices](../hen-updater/DEPENDENCIES.md).

The plugin-manager/lifecycle changes originated in supplied Project 2 source snapshots, including correction revision 2026112502, then were integrated with the HEN-only updater and revised to manager build 2026112505. The corrected input archive SHA-256 is `5fd69becbd0864834473deb67e009cb6f9b8288a18a556ceea8f0d7bfb6d0167`. This records source origin; private handoff documents and conversations are not included. No upstream authorship is claimed.

The example C sources and Makefiles carry **MIT** markers, retained unchanged. [MIT text](LICENSES/MIT.txt). No additional copyright-holder notice was supplied with those example files. The lifecycle/runtime API retains ISC markers and the existing [libkaihen ISC notice](../hen-updater/libkaihen/LICENSE). Test shims, fixtures and original artwork retain any file-level notices; they do not change the shared runtime's license.

The plugin NRO and ping NSO link libounce/newlib/GCC runtime code. Their notices are retained alongside the core binaries in [hen-updater/LICENSES](../hen-updater/LICENSES/), including libounce ISC, libnx ISC, libkaihen ISC, mimalloc MIT, newlib's notice collection and the GCC runtime exception/GPL text. Keep those notices with redistributed binaries. The authored SVG/JPEG icon is included with its rendering source; no third-party game artwork is used.

The repository's Unlicense does not waive any of these third-party terms. No Nintendo game, firmware, save, signed policy, proprietary SDK or generated ExeFS package is bundled.
