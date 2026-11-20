# Dependencies and source references

The runtime, policy, UI, tests, and scripts were written for this tool with AI
assistance. Recognition follows the shape seen in retained FS-attempt tree
captures and earlier narrowly targeted cleanup operations. Private captures,
save contents, account identifiers, and operation logs are not included.

| Dependency | Use | Reference |
| --- | --- | --- |
| devkitA64 / devkitPro tools | Cross compiler, NACP creation, NRO conversion | [devkitPro](https://devkitpro.org/wiki/Getting_Started) |
| ounce-devkit / libounce | Public homebrew filesystem, account, UI and hashing APIs | [ounce-devkit](https://github.com/ouncebrew/ounce-devkit), [libounce](https://github.com/ouncebrew/libounce) |
| libkaihen | Initialized homebrew filesystem-root access | [Homebrew interface](https://github.com/ouncebrew/kaihen/blob/main/hen/README.md), [client API](https://github.com/ouncebrew/kaihen/blob/main/libkaihen/include/kaihen.h) |
| Zig | Host C tests | [Zig](https://ziglang.org/) |
| CairoSVG and Pillow | Render the authored SVG to the NRO's JPEG icon | [CairoSVG](https://cairosvg.org/), [Pillow](https://python-pillow.github.io/) |

The original build used GCC 16.1.0 and an ounce-devkit archive with SHA-256
`33cc5478324f8243dc801c3d4b8fe781885a306975f63937bd246ffdfd73cf79`.
Its exact upstream source revision was not recorded. The hash identifies an
input, not compatibility with current packages or alternate SDK layouts.
The icon was rendered with CairoSVG 2.8.2 and Pillow 12.2.0.

The public libkaihen client library uses ISC terms, separate from kaiHEN's
top-level project license. Notices supplied with the SDK libraries are retained:

- [libkaihen ISC](licenses/libkaihen-ISC.txt)
- [libounce ISC](licenses/libounce-ISC.txt)
- [libnx ISC](licenses/libnx-ISC.txt)
- [mimalloc MIT](licenses/mimalloc-MIT.txt)

SDK source, headers, library archives, and toolchain executables are not
separately bundled. The NRO contains linked runtime code.
Original work follows the repository Unlicense; dependency licenses and any
contributions they make to a built executable retain their own terms.

## Included NRO

The exact maintainer-tested executable is included in this directory.
[NRO notices](NRO-NOTICES.md) identify it and retain the SDK, C-library, and
compiler-runtime notices. The repository Unlicense does not waive those
third-party rights.
