# Seedsow FS cleanup

A small kaiHEN NRO for removing the recognizable scratch folders left by
Seedsow FS attempts. Those folders can contain an 8 MiB scratch file and a
chain of directories too long for backup tools to traverse. I made this after
running into failed save backups in my own workflow.

Version **0.1.2** worked on my console. This is largely vibecoded developer
reference, with no guarantee of function, compatibility, safety, or support.
[Observed results and limits](TESTING.md).

## What it does

The app scans supported Seedsow account saves read-only and shows the profiles
it can inspect. Cleanup applies to **one selected profile**. It initially
selects a profile with recognized leftovers, but does not clean automatically.

After confirmation, it copies files outside the recognized scratch trees to
SD, checks their hashes, checks the selected save again, deletes only those
trees, and commits once. A fresh read-only mount then checks that the trees
are absent and the retained files still match their original hashes.

It does not restore saves or replace launcher scripts. A successful cleanup
may let Checkpoint or JKSV make a backup where long scratch paths previously
prevented one. Other backup failures can have different causes.

## What counts as a scratch tree

Recognition requires a root directory named `NS2_GAP_` followed by exactly 13
decimal digits, plus the known probe layout:

- `commit_data`, a nonempty file no larger than 512 MiB;
- `canary`, empty or containing a single chain of directories each named with
  exactly 32 uppercase `A` characters;
- `S0_<timestamp>/S1_<timestamp>/S2_<timestamp>/SPAN_MARKER`, using the same
  timestamp as the root, with a one-byte marker file;
- optionally an empty `selected` directory, with no unknown root entries.

The known canary reaches beyond the filesystem API's 768-byte path limit.
The recognizer checks the chain up to that boundary; it cannot inspect its
deeper contents. Cleanup removes the whole confirmed tree, including that
unreachable portion. Its contents are excluded from the preservation copy.

File size or an `NS2_GAP_` prefix alone is insufficient. Different layouts are
left alone. If an unknown folder prevents preserving the other files, cleanup
stops before deletion.

The title allowlist contains Seedsow Lullaby Demo (`010042D022AEA000`) and the
full game (`0100EE5021C9E000`). Only User-space primary account saves with
index zero are eligible. The full-game variant has not been console-tested.

## Use

With kaiHEN already running, launch the NRO from its own location under
`/switch/` using hbmenu. Close other save tools before cleaning.

1. Select the profile with Up/Down and press **A** to review its folders.
2. Check the profile name and folder list. Hold **L + R**, then press **A** to
   confirm cleanup for that profile.
3. After a verified result, choose that **same profile** in your backup tool.

**B** goes back; **X** rescans the list; **+** exits while idle. The app has no
network command interface.

This writes to an entrypoint save. A failed or incorrect operation can corrupt
that save or break its entrypoint. The SD copy is not a guaranteed recovery
method, particularly if homebrew access is lost. Recovery, ban safety, and
compatibility with other setups have not been established.

## SD preservation copy

Copies are stored under `/switch/seedsow-fs-cleaner/backups/`, in a unique
directory for the selected save and run. `files/` mirrors retained files;
`manifest.tsv` records paths and hashes; `operation.log` records the stages.
The records contain that console's account/save identifiers and file names.

The app checks both the copied bytes and a second read of the source files.
It also checks the manifest after flushing it. This is a partial save copy,
not a complete archive or a tested automatic restore mechanism.

The operation is bounded to 16 recognized trees, 4,096 retained entries
including the root, and 512 MiB of retained file bytes. SD path limits,
insufficient space, read errors, or changed files stop cleanup. There are no
automatic retries. An uncertain write disables further cleanup in that app
session so the save can be inspected first.

## Build and tests

The Windows PowerShell wrapper expects your own devkitA64/devkitPro tools and
an ounce-devkit layout with libounce and libkaihen:

```powershell
./build.ps1 -SdkRoot 'PATH_TO_OUNCE_DEVKIT' -DevkitProRoot 'PATH_TO_DEVKITPRO'
./test-local.ps1 -ZigCompiler 'PATH_TO_ZIG'
```

The NRO and its build manifest are written under `build/`. Fresh builds are
marked untested. The included [seedsow-fs-cleaner.nro](seedsow-fs-cleaner.nro)
is the maintainer-confirmed 0.1.2 build. SDK components and console data are not
included.

The primary icon is [hand-written SVG](assets/icon.svg). `render-icon.py` uses
CairoSVG and Pillow to regenerate the JPEG required by the NRO format; the
pre-rendered JPEG is included. [Dependencies and notices](DEPENDENCIES.md).

Original work follows the repository's [Unlicense](../UNLICENSE). The C
implementation was written with AI assistance; the icon was authored as SVG
without image diffusion. Dependency rights and notices remain separate.
