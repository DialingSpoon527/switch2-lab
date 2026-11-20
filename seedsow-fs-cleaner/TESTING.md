# Observed use of 0.1.2

The maintainer confirmed that version 0.1.2 worked for their requested cleanup
on November 19, 2026. That confirmation followed installation and full SD
readback verification of the exact NRO below. No completion log for this
particular run was retrieved, and successful backup or save restoration was
not separately confirmed. This is one console, not a compatibility study.

The environment used kaiHEN homebrew protocol 7, with firmware reported as
23.0.1. Firmware was not independently verified by the cleaner. The build
used devkitA64 GCC 16.1.0 and the ounce-devkit input identified in
[DEPENDENCIES.md](DEPENDENCIES.md).

## Console evidence and version distinction

For an earlier 0.1.0 run, an operation log and matching installed NRO hash
showed one profile's scratch tree deleted, a successful commit, and a fresh
read-only verification of 12 retained files. That earlier trace is context,
not independent proof of every 0.1.2 code path.

Version 0.1.2 corrected a profile-validation false rejection. In the observed
case, the ID returned inside a profile-details record differed from the ID
used for its active account lookup, while the active ID matched save ownership.
The cause of that record discrepancy was not established and no account data
was edited.

Selection now intersects the active account list with filesystem save ownership.
It never substitutes the profile-details record's ID as the cleanup target.
The active account and exact save ID are rechecked before preservation and
before deletion. Missing or duplicate active-account matches remain refusals.

## Offline checks

The host tests cover recognition of the known tree shape, timestamp agreement,
unknown content and read-error rejection, and the long-canary boundary. They
also cover profile-record mismatches, inactive or duplicate accounts, path
bounds, save scope, and stopping at each failed transaction stage without
automatic retries.

The transaction tests use mock operations. They do not simulate all filesystem
behavior, power failure, or damaged storage, and do not replace console tests.
The portable build wrapper and synthetic-fixture test results are separate
from the maintainer's confirmation of the original console build. A new
cross-built executable has not thereby been console-tested.

## Tested build identity

The original confirmed NRO SHA-256 was
`86b0400149d07fb4f0aef6aaeeda77cc9bb460a090f8f9aedc103c1aab139429`.
That exact build is included as [seedsow-fs-cleaner.nro](seedsow-fs-cleaner.nro). The runtime C files and headers match that build's
recorded sources. Test timestamps were replaced with synthetic fixture values;
build/test wrappers accept external toolchain paths.

Different paths, toolchains, or dependencies can produce a different executable.
Matching version text alone does not establish that a build is identical.

## Limits

The observed console use involved the Demo. Full-game support is allowlisted
from known title identification, but was not console-tested. Other firmware,
HEN versions, probe layouts, backup-tool failures, recovery paths, concurrency,
and long-term reliability remain unestablished. The tool does not establish
safe recovery or protection from online-service restrictions.
