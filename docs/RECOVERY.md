# Recovery

Everything here is about getting back to a clean SmileBASIC + 4/4 session.  The
launcher only ever runs after `4/4 verified`; if the console is not at 4/4,
nothing in this repository touches it.

## Rules

* **The Pico cannot press the physical power button.**  If the console is hung
  or its screen is black, a human has to hold the power button (see below).
* The exploit state is per app launch.  After a crash, close and relaunch
  SmileBASIC; HOME-resume is not enough.
* Do not flash the Pico while it is connected to the console.
* Never write NAND or firmware.

## Bad state → recovery

### 1. Black screen / no HDMI signal (what this session saw)

The console is asleep or off.  The host cannot wake it.

1. Press and release the **power button** on the console (or any controller
   button if the console is merely asleep).
2. If the screen stays black for ~30 s, hold the power button for **12 s** to
   force a shutdown, then press it once to power on.
3. Confirm the Elgato shows the HOME screen:
   `python3 ../pico-pro-controller/tools/capture_frame.py` and look at
   `tools/capture/screen.png`.
4. Continue with section 3.

### 2. Crash dialog: "There was a problem, and the software needed to close."

Follow the known-good sequence from `pico-pro-controller/RUNBOOK.md` with
`HBL_PICO_TOOL` pointing at `pico_ctl.py`:

| # | action | wait | why |
| --- | --- | --- | --- |
| 1 | `tap A` | 3 s | dismiss the crash dialog |
| 2 | `tap HOME` | 1 s | go to HOME |
| 3 | `tap X --ms 250` | 1 s | open "Close the software?" |
| 4 | `tap A` | 3 s | confirm close |
| 5 | `tap A` | 15 s | launch SmileBASIC |
| 6 | `tap A` | 3 s | dismiss Information |
| 7 | `tap A` | 3 s | dismiss the DNS error |
| 8 | `tap A` | 3 s | dismiss the tag error |
| 9 | `tap A` | 4 s | TOPMENU: Make a Program |
| 10 | `text load "txt:ns2"\n` | 3 s | LOAD the bootstrap |
| 11 | `tap A` | 3 s | confirm Yes |
| 12 | `text run\n` | 8 s | RUN the bootstrap |
| 13 | `text remove nsr,&h60000000,&h3fffffff\n` | 4 s | free the NSR region |
| 14 | `text v=start(39380,10,42,0,1)\n` | 8 s | start the RPC session |

`./hbl-start --guide` performs exactly this sequence through the Pico.

### 3. Verifying 4/4

Watch the runner log and the Pico status page:

```sh
tail -f /tmp/hbl-runner.log            # host-side stages
curl -s http://192.168.4.30:8099/ | grep -o 'chainBadge[^<]*'
```

Required lines: `1/4`, `2/4 Acquiring FS memory`, `3/4 Starting the native
helper`, `4/4 Running the FS demo`, **`4/4 verified`**.

`./hbl-start --preflight` reports the current stage for the newest run.

### 4. `3/4` fails with `find-metadata`

The application was not freshly started.  Repeat from step 2 (`tap HOME`) and
relaunch SmileBASIC.  If it fails again, power-cycle the console.

### 5. `run` does not print `DIST BOOT READY 1015`

The Pico's keyboard occasionally drops a short packet.  Send `run\n` once more
(`python3 "$PICO" text $'run\n'`).  If that still fails, restart from step 2.

### 6. Launcher entry fails (status 4 / `HBX_ERR_GPU`)

Symptoms: the screen keeps the last frame (or stays white) and the session log
shows `hbl: display create failed` or `hbl: present failed`.

1. Read `build/hbl-session.log` — the exact stage and VkResult are logged.
2. A present failure usually means the swapchain was invalidated (the app or
   another presenter took the vi layer).  Restart SmileBASIC and re-acquire 4/4.
3. Run `node host/probe.cjs <run-dir>`: if the probe fails too, the problem is
   mapping/ABI, not Vulkan (the probe does no graphics at all).
4. A display-create failure at `vkCreateViSurfaceNN` or swapchain creation
   means the vi window is stale: close SmileBASIC fully (HOME → X → Close) and
   start a fresh session.  A retained `vi-layer.json` from an older session is
   ignored automatically when the session id differs.
5. If it still fails, re-run the archive display inspection
   (`sm64-console/host/inspect-display.cjs <run>`) to confirm the display
   stack, then retry.

### 7. The launcher exits but the screen stays on the last frame

Expected.  The launcher never tears down the vi swapchain (that is the proven,
non-crashing policy).  Restart SmileBASIC to reclaim the display.

### 8. The console is unresponsive to the Pico

1. `python3 "$PICO" state` — clear any stuck button.
2. Power-cycle the console (section 1) and re-run the sequence.
3. If the Pico itself is unresponsive, reset it **only while it is disconnected
   from the console**; never flash it while connected.

## Where the evidence lives

| artifact | path |
| --- | --- |
| host session log | `build/hbl-session.log` |
| runner log | `/tmp/hbl-runner.log` |
| run artifacts | `$HBL_NS2JB/runs/<session>/` |
| app/launcher log ring | drained into `build/hbl-session.log` after each entry |
| capture | `../pico-pro-controller/tools/capture/screen.png` |

Never loop blindly after a crash: preserve the logs, fix offline, then retry
from a fresh SmileBASIC launch.
