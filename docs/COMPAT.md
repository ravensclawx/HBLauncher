# Optional legacy compatibility adapters

The host retains adapters for external NES and SM64 loaders. No compatibility
app packages, game ports, game data, or icons are shipped in this release.
The included demos use the native SDK path instead.

To use an external compatibility project, provide your own package manifest
and dependencies. `HBL_COMPAT_ROOT` points to a directory containing the
external `nes-port` or `sm64-console` checkout. `HBL_NS2JB` supplies the live
session artifacts. See `host/lib/compat.cjs` for the driver contract.

A manifest's `driver` block may provide a loader command and arguments;
`{run}` in an argument is replaced with the live run directory. External
loaders run as child processes and own the console session until they return.

These adapters are experimental. Shared display ownership, loader failures,
and external-project compatibility require hardware verification. Restart
SmileBASIC after a failed session; see [Recovery](RECOVERY.md).
