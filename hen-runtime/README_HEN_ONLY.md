# Core build helper output

This output belongs to the combined kaiHEN runtime, manager build 2026112505,
based on upstream 20261123. It contains freshly built files, not the previously
console-tested downloads. New builds have no inherited hardware result.

`first-install/` contains the core bootstrap, RO, HEN, supervisor and updater app,
plus the selected official companion files. Match them to your acquisition or
untethered launcher. The standalone package layout keeps bootstrap.bin and
ro/main outside sdcard/; an untethered launcher may require different paths.
Preserve a working entrypoint and matched files before changing startup inputs.
The separate Plugin starter/ping example binaries are not packaged by this helper.

`hen-update/` contains a candidate HEN under sdcard/kaihen/hotupdate/staged/hen/main
and its checksum manifest. Copying sdcard/ does not queue a replacement. The
one-shot command examples are deliberately outside sdcard/.

To request one replacement with the matching supervisor already running:

1. Stop other managed plugins and confirm they exited. Keep the supervisor.
2. Copy the candidate sdcard/ contents to SD and finish transfers.
3. For status only, open HEN updater without a command file.
4. For a dry run, copy commands/DRY-RUN.txt to SD as
   /kaihen/hotupdate/hen-command.txt, then open HEN updater.
5. For an actual replacement, use commands/APPLY.txt at that same path.
6. After the app reports the request queued, leave it and close the actual
   host game from HOME. Leaving the NRO alone does not close the host process.
7. Wait for the completed transaction result before opening another host.
   A queued request is not a successful handoff. Preserve reports on uncertainty;
   do not blindly repeat a timed-out operation.

Plugin launches can keep a stable host open; HEN replacement still needs host
exit. Plugin registrations are not migrated across HEN generations. A failed
privileged operation can lose HEN or leave uncertain FS state. SD backups do not
guarantee recovery from persistent-storage damage. Different-version upgrades
and ban safety have not been established.

`source/` contains the full matching source, detailed guides, test descriptions
and component licenses. The runtime is a GPL-2.0-only derivative with separately
licensed API/runtime components and examples. Retain applicable notices.
