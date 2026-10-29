# HBLauncher Native SDK

Build real CPU + Vulkan apps for the launcher.  An app is a freestanding
AArch64 image with two parts (`code.bin` RX, `data.bin`/`.bss` RW) plus a
manifest, assets and an icon.  The launcher maps it, hands it a versioned
context, and gets control back when it returns.

## Five minute walkthrough

```sh
# create an app from the template
python3 sdk/tools/hbx-new.py apps/my-app "My App"

# edit apps/my-app/src/app.c  (entry point: hbx_app_main)
python3 sdk/tools/hbx-build.py apps/my-app
./hbl-start --mock            # the new title appears in the catalog
```

A minimal app:

```c
#include "hbx_sdk.h"
#include "hbx_app.h"
#include "hbx_ui.h"

int hbx_app_main(const struct hbx_ctx *ctx) {
    struct hbx_input_state input;
    uint64_t deadline = 0;
    if (hbx_ctx_check(ctx) != HBX_OK) return HBX_ERR_ABI;

    while (!hbx_stop_requested(ctx)) {
        hbx_input_poll(ctx, &input);
        if (input.pressed & HBX_B) break;

        hbx_ui_begin(ctx, HBX_RGB(250, 252, 254));
        hbx_ui_text(ctx, 120, 120, HBX_FONT_H1, "Hello from the SDK", HBX_RGB(26, 69, 102));
        hbx_ui_present(ctx);
        hbx_frame_end(ctx);
        hbx_pace(ctx, &deadline, 60);
    }
    return HBX_OK;
}
```

## Package layout

```
apps/<id>/
  manifest.json
  src/*.c, *.S
  assets/...          optional, exposed via hbx_asset()
  icon.png            96x96 (optional; a letter tile is used otherwise)
  build/              code.bin, data.bin, report.json (generated)
```

`manifest.json`:

```json
{
  "id": "my-app",
  "name": "My App",
  "description": "One line shown under the title",
  "version": "1.0.0",
  "abi": 3,
  "kind": "native",
  "assets": ["levels/one.bin"],
  "icon": "icon.png",
  "limits": {"heap_bytes": 4194304, "stack_bytes": 1048576, "frames": 3600}
}
```

`kind` is `native` for SDK apps.  `nes` and `sm64` are compatibility packages
driven by their own proven loaders (see `COMPAT.md`).  `frames` is a safety
budget for one entry.

## Build

```
sdk/tools/hbx-build.py apps/my-app [--code-base 0x09000000]
                                    [--data-base 0x0B000000] [--clean]
```

Defaults: code at `0x09000000`, data at `0x0B000000` (the launcher owns
`0x08000000`/`0x0A000000`).  Both must be page aligned and must not overlap the
launcher.  The tool compiles `src/*.c|S` with the freestanding flags, links
against the SDK runtime, and verifies the entry, `hbx_app_main`, and that the
image has no unresolved symbols.  Output `build/report.json`:

| field | meaning |
| --- | --- |
| `codeBase`, `dataBase`, `entry` | fixed addresses |
| `codeBytes`, `codePages`, `dataBytes`, `bssBytes`, `dataRegionBytes` | sizing |
| `codeSha256`, `dataSha256` | integrity |

The loader checks the hash before mapping and refuses stale builds.

## The context

```c
struct hbx_ctx {
  uint32_t magic, abi_version, size, flags;
  uint64_t status, exit, frame;          // out
  uint64_t now_ns, tick_hz, uptime_ns;   // time at entry
  uint64_t heap_base, heap_size;
  uint64_t data_base, data_size;         // package assets
  uint64_t code_base, code_size;
  uint64_t stack_base, stack_size;
  uint64_t control, catalog, catalog_bytes, icons, assets, log, ui;
  uint64_t atlas, atlas_bytes;
  struct hbx_gpu gpu;
  struct hbx_input input;
  uint64_t args[8];
};
```

`sdk/abi.json` is generated from the C headers (`tools/gen_abi.py --check`) and
is what the host loader trusts.  Offsets are static-asserted in `hbx_sdk.h`.

## Services

### Input

`hbx_input_poll()` ORs the handheld, full-key and dual-joy pad sources into
`buttons`, computes `pressed`/`released` edges, applies a 15% radial deadzone and
rescales the sticks to −100..100.  Touch points arrive in 1280x720 space and are
scaled 3:2 by `hbx_touch_hit()`.

`hbx_input_nav()` maps D-pad/stick to a menu direction plus confirm (A, Plus,
right-stick click, ZR) and cancel (B, Minus, ZL).

Buttons: `HBX_A`, `HBX_B`, `HBX_X`, `HBX_Y`, `HBX_L/R`, `HBX_ZL/ZR`,
`HBX_PLUS/MINUS`, `HBX_LEFT/UP/RIGHT/DOWN`, `HBX_STICK_L/R`.

### Time and pacing

`hbx_now_ns()` is monotonic across entries; it anchors to the host-provided
session time on the first call.  `hbx_pace(ctx, &deadline, 60)` advances an
absolute deadline and sleeps only the remaining time; if a frame overruns it
resynchronises instead of bursting.

### Heap

`hbx_heap_init(&heap, base, bytes)` over the host-provided arena;
`hbx_alloc`, `hbx_alloc_aligned` (up to 4096), `hbx_free`, `hbx_heap_peak`.
Boundary-tag first-fit with forward/backward coalescing; payloads 16-byte
aligned; double frees and foreign pointers are ignored.

### Assets

Every file listed in `manifest.assets` is packed by the loader into the data
region and exposed through `hbx_asset(ctx, "levels/one.bin", &size)`.

### Logging

`hbx_log(ctx, "...")` and `hbx_logf(ctx, "x=%d %s", ...)` write newline-
terminated lines into a host-visible ring.  The host drains it after every
app entry, so failures are visible in `build/hbl-session.log`.

### Exit

Press/act → `return HBX_OK`, or call `hbx_request_exit(ctx)`.  The host unmaps
the image, frees its blocks and re-enters the launcher.

## UI

`hbx_ui.h` exposes the launcher's 2D renderer as a call-through table:
`hbx_ui_begin/rect/stroke/text/text_width/line/circle/icon/present`.

* Coordinates are 1920x1080 pixels, origin top-left, +y down.
* Colours are `0xAABBGGRR` — use `HBX_RGB(r,g,b)` / `HBX_RGBA(r,g,b,a)`.
* Fonts: `HBX_FONT_TITLE` 88 px bold, `H1` 34 bold, `H2` 26 bold, `BODY` 20,
  `SMALL` 17, `TINY` 14.
* `hbx_ui_present()` acquires a swapchain image, records one draw of all
  accumulated geometry, presents and waits for the queue.
* `begin(ctx, colour)` clears the frame to `colour`.

The advanced escape hatch `ops->vertices` takes triangles in the same vertex
format the renderer uses; see `apps/gpu-triangle`.

### Raw Vulkan

`ctx->gpu` hands you instance/physical/device/queue, the swapchain images and
views, the format, and `struct hbx_vk_dispatch *` (`hbx_vk(ctx)`).  Create
pipelines, buffers, descriptors and command pools through the table, draw into
`gpu.views[index]`, and present with your own semaphores.  Do **not** destroy
or recreate the swapchain, the views or the surface; the launcher owns them.
`sdk/examples/raw-vulkan/` is a complete example.

`sdk/include/hbx_vk.h` documents the 72-entry table.  The host rebuilds it for
every session from the console module's exports, so never cache addresses.

## Compatibility packages

`kind: "nes"` and `kind: "sm64"` packages are listed and launched by the
launcher but executed by their proven loaders for a finite session.  See
`COMPAT.md`.

## Limits and rules

* No libc, no dynamic linking, no writable globals outside the mapped data
  region.  The SDK runtime is freestanding.
* Keep the image position-dependent: link at the package's code base.
* Do not touch `ctx` fields you do not own; treat `hbx_ctx` as read-only
  except `status`, `exit`, `frame`.
* One frame budget per entry; return to the launcher when the user asks.
* Test offline: `python3 sdk/tools/hbx-build.py <app>` then
  `./hbl-start --mock`; the build is deterministic.

### Package extras

* **Asset globs.** An `assets` entry of the form `"*.ext"` packages every file
  with that suffix directly in `assets/` (sorted; may match nothing). At most
  64 assets in total.
* **Defines.** `"defines": ["NAME", "NAME=VALUE"]` adds `-D` flags to every
  source (the NES app uses `AUDIO=1`).
* **Save block.** `"save_bytes": N` (up to 16 MiB) gives the app a RAM block at
  `ctx->args[6]` (size in `ctx->args[7]`, or use `hbx_save_block()`), preloaded
  by the host from `saves/<id>.sav` on the PC. Start it with a
  `struct hbx_save_header`. When the app returns with `dirty` set, the host
  writes the first `bytes` bytes back, keeping the previous file as `.bak`.
  Nothing is written on the console. `HBL_SAVES_DIR` overrides the folder.
