# Plugin auto-start

Plugins in `/kaihen/plugins/NAME.nso` still start automatically by default.
Installing an ordinary third-party plugin preserves that behavior. This bootstrap
adds two ways to keep selected plugins installed for on-demand use only.

## A marker beside the plugin

Create `/kaihen/plugins/NAME.no-autostart` alongside `NAME.nso`. Its presence
suppresses automatic startup; its contents are ignored. An installer can ship
this small marker without editing a shared configuration file. For example:

```text
/kaihen/plugins/my-plugin.nso
/kaihen/plugins/my-plugin.no-autostart
```

Remove the marker to restore auto-start, provided the plugin is not also in the
blacklist below. A marker does not stop an already-running process or prevent
an explicit on-demand launch.

## An optional blacklist

Put plugin names, without `.nso`, in `/kaihen/plugins/no-autostart.txt`:

```text
# Keep these installed, but launch them only when requested.
p2ping
my-plugin
```

A missing or empty blacklist excludes nothing. Lines may have `#` comments,
spaces or tabs around a name; LF, CRLF and a leading UTF-8 BOM are accepted.
Names are case-sensitive ASCII letters, digits, underscores or hyphens, up to
63 characters. Up to 64 distinct names fit in a file of at most 4096 bytes.
Duplicate names have no additional effect. Paths, extensions, NUL bytes and
malformed names are rejected. [Example file](../packaging/no-autostart.example.txt).

A blacklist entry **or** a marker suppresses automatic startup. A malformed or
unreadable blacklist stops plugin auto-start for that boot and writes a diagnostic
where SD access permits; it never falls back to launching everything. HEN still
attempts its normal game-arming step. An unexpected marker lookup error stops
the plugin scan instead of starting the uncertain plugin.

## Runtime and update behavior

The bootstrap reads these controls once at startup. It records skipped plugins
as `auto-start disabled; available on demand` in `/kaihen/plugin-status.txt`.
Their PIDs are zero because no process was started. HEN's on-demand launch path
continues to use `/kaihen/plugins/NAME.nso`; it does not consult the blacklist or
marker. Keep that NSO in place if an app needs to launch it later.

Leave `hotupdate-supervisor` enabled if you want the HEN replacement feature.
Disabling it leaves that supervisor unavailable during the session. Ping is an
optional test plugin. No arbitrary plugin compatibility is established by these
controls, and an enabled plugin can still stall startup or lose the HEN session.

This changes the native bootstrap, with no HEN/RO/supervisor protocol change.
It takes a fresh bootstrap run to apply the new code; the HEN updater does not
replace the running bootstrap. Old bootstraps ignore these controls. Reverting
to one while retaining on-demand-only NSOs in the auto-start directory would
launch them again, so park those NSOs outside the directory before reverting.

Build the bootstrap with the matched runtime source and SDK. Host checks are
available with `sh bootstrap/test.sh`; they exercise the real parser with
sanitizers and random inputs, but do not emulate console IPC or prove hardware
recovery. [Test observations](../TESTING.md).
