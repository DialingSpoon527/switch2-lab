# Dependencies and source references

This directory contains the bridge's original implementation and generated icon.
SDK headers, libraries, upstream source trees, compiler binaries, and console
inputs are not bundled. The host layout test calls the installed SDK's CMIF
serializer instead of embedding a copy of that implementation.

| Dependency | Use | Reference |
| --- | --- | --- |
| devkitA64 / devkitPro tools | Cross compiler, NACP generation, NRO conversion | [devkitPro](https://devkitpro.org/wiki/Getting_Started) |
| ounce-devkit | Public Switch 2 development environment | [ouncebrew/ounce-devkit](https://github.com/ouncebrew/ounce-devkit) |
| libounce | Homebrew services, networking, input/display, JIT and IPC helpers | [ouncebrew/libounce](https://github.com/ouncebrew/libounce) |
| libkaihen | kaiHEN homebrew client API | [API header](https://github.com/ouncebrew/kaihen/blob/main/libkaihen/include/kaihen.h), [homebrew interface](https://github.com/ouncebrew/kaihen/blob/main/hen/README.md) |
| Node.js | PC command client and mock tests | [Node.js](https://nodejs.org/) |
| Zig | C host-test compiler | [Zig](https://ziglang.org/) |

The build used devkitA64 GCC 16.1.0 and an earlier ounce-devkit archive with
SHA-256 `33cc5478324f8243dc801c3d4b8fe781885a306975f63937bd246ffdfd73cf79`.
The exact SDK source revision for that archive was not recorded. This hash
identifies the input used; it is not a claim about current package compatibility.
Current upstream installation instructions may describe a newer layout.

Public libounce API references were reviewed from revision
`c697effbe71aa4c9784063a902c3b8b95cb528bc`. The source-reference checkout and
the SDK archive are separate inputs; this does not assert they were built from
the same revision.

The linked libkaihen client library uses ISC terms; kaiHEN's top-level project
license is separate. The SDK's libounce and libnx ISC notices are retained here:

- [libkaihen notice](LICENSES/libkaihen-ISC.txt)
- [libounce notice](LICENSES/libounce-ISC.txt)
- [libnx notice](LICENSES/libnx-ISC.txt)

The ounce-devkit build/install project has its own MIT license. Other components
in a developer's toolchain retain their own licenses. The repository Unlicense
applies to original work here and does not relicense those dependencies or their
contributions to a built executable.
