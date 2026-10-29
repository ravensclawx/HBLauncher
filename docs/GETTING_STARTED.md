# Getting started

This walks from a fresh checkout to the launcher on the console, and from there
to your own app.  If something is already broken, read `RECOVERY.md` first.

## 0. Prerequisites

| need | why |
| --- | --- |
| Node 18+ (`HBL_NODE` if not on `PATH`) | host loader, preflight, tests |
| `gcc-aarch64-linux-gnu` + binutils | freestanding images |
| Python 3 + Pillow | font atlas and icon generation |
| `glslang-tools`, `spirv-tools` | shader blobs (only when regenerating) |
| `ns2jb-kurumuz` checkout | exploit chain and run directories (`HBL_NS2JB`) |
| Pico Pro Controller + capture card | driving SmileBASIC, screenshots |

The SDK itself only needs the AArch64 toolchain; the console session needs
everything else.

## 1. Build and test offline (no console)

```sh
sh tools/build-all.sh     # assets, launcher, apps, example, preview
sh tests/run.sh           # ABI/runtime, host lifecycle, real-Vulkan UI tests
./hbl-start --mock        # full lifecycle against a scripted backend
```

`--mock` prints the launch → app-return → resume → exit sequence with the same
code paths the console uses.  It is the fastest way to check a change.

The preview binary renders the actual launcher screen on the workstation:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/preview/hbl-preview tests/preview/out 6
# PPM frames land in tests/preview/out, one per state (library, selected,
# launching, returned, empty)
```

## 2. Prepare the console session

1. Power the console, let it reach HOME, and start SmileBASIC fresh.
2. On the workstation, make sure the wired link is up:
   `sudo ip addr add 10.42.0.1/24 dev <iface>` and `ip link set <iface> up`.
3. `./hbl-start --preflight` — every required check must be green; optional
   ones (console RPC, 4/4 session, Pico, capture) tell you what is missing.
4. Start the runner and drive the bootstrap:

   ```sh
   ./hbl-start --spawn --guide
   ```

   `--guide` taps and types the RUNBOOK sequence through the Pico.  Without
   `--guide`, follow the steps printed by the command (also in `RECOVERY.md`).
5. Confirm the session reached **4/4 verified** (`--preflight` shows the stage,
   the Pico status page shows the chain badge).

## 3. Launch

```sh
node host/probe.cjs <run-dir>         # mapping/ABI smoke test, no display
./hbl-start --entries 1 --frames 60   # map the launcher, draw one second, stop
./hbl-start                           # normal session
```

The probe is the first thing to run on a new firmware or after touching the
loader: it maps a 3.6 KB image at `0x09000000`, publishes its cache lines, calls
it, and checks the return value, a marker written into the context and the log
ring.  If the probe succeeds but the launcher does not, the problem is in the
Vulkan bring-up, not in mapping or the ABI.

What happens: the host creates/retains the vi layer, builds the Vulkan dispatch
table from the session module, maps the launcher at `0x08000000` with its data
at `0x0A000000`, publishes code (one patch per 64-byte line), and calls the
entry.  The launcher creates the display, draws the wave screen, and returns to
the host every 12 seconds or when you pick a title.

Controls: **D-pad / left stick** move, **A** open, **B** back, **+** about,
**−** exit the launcher session, **touch** a row to select, tap again to open.

## 4. Build your own app

```sh
python3 sdk/tools/hbx-new.py apps/my-app "My App" "One line about it"
# edit apps/my-app/src/app.c
python3 sdk/tools/hbx-build.py apps/my-app
./hbl-start --mock            # it appears in the catalog
```

Full SDK guide: [`SDK.md`](SDK.md).  Start from `apps/hello-cpu` for CPU logic,
`apps/gpu-triangle` for GPU geometry, `sdk/examples/raw-vulkan` for an
app-owned pipeline.

## 5. Reading the result

| file | contents |
| --- | --- |
| `build/hbl-session.log` | every stage, timing, status and the app log ring |
| `$HBL_NS2JB/runs/<session>/` | exploit artifacts and `vi-layer.json` |
| `/tmp/hbl-runner.log` | runner output when started by `--spawn` |

If an entry fails, the log names the stage (`instance`, `vi-surface`,
`swapchain`, `ui-init`, ...) and the VkResult.  That is the first thing to put
in a bug report.

## 6. After a session

The launcher is **resident** for the SmileBASIC session: code, data, display
and shared blocks stay mapped so a later `hbl-start` reuses them instead of
creating a second display.  Restart SmileBASIC to reclaim the memory and the vi
layer.  See `RECOVERY.md` §7.
