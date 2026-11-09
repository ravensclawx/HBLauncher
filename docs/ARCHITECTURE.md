# Architecture

## The shape of the thing

```
 SmileBASIC process (EL0)                     workstation
 ┌──────────────────────────────────────┐     ┌────────────────────────────┐
 │  host thread (call-address)          │     │ hbl-start                  │
 │   └─ _start(launcher) ──────────────┐│     │  ├─ preflight              │
 │       Vulkan instance/device/queue  ││     │  ├─ session (4/4 gate)     │
 │       vi surface + swapchain        ││     │  ├─ vi layer (NN)          │
 │       CPU UI -> vertex buffer       ││◀───▶│  ├─ dispatch table         │
 │       one vkCmdDraw + present       ││ RPC │  ├─ loader (map RX/RW)     │
 │   └─ return: launch request ────────┘│     │  └─ app lifecycle          │
 │  host thread (call-address)          │     └────────────────────────────┘
 │   └─ _start(app package) ───────────┐│
 │       ctx->ui table -> same renderer││
 │   └─ return: exit status ───────────┘│
 └──────────────────────────────────────┘
```

The console runs one EL0 thread of the SmileBASIC process.  The host
workstation drives it over RPC; `call-address` blocks until the called image
returns.  The launcher therefore runs in **bounded entries**: each call runs a
frame budget (default 720 frames = 12 s) or returns early to ask the host to
launch an app.  The display keeps the last presented frame between entries, so
app launches and returns look continuous.

## Memory map

| range | size | contents | permissions |
| --- | --- | --- | --- |
| `0x08000000` | 47 KB (14 pages) | launcher code + rodata | RX |
| `0x09000000+` | per app | app code + rodata | RX |
| host blocks | — | ctx, control, log, heap, stack, dispatch, catalog, icons, atlas, assets, persist | RW |

Host blocks are allocated with the session SDK allocator and are only visible
to the images through absolute pointers in `struct hbx_ctx`.  The ns2jb
payload accepts command IDs `0x524f0010`–`0x524f0016` only, so images are
placed with the single-page primitives:

```
0x524f0012  map one page RX from a page-aligned Normal/RW source into a free page
0x524f0013  unmap one page
0x524f0014  write 16 bytes with cache publish + readback
```

`0x524f0012` finalises the destination **RX** (`process_permission 5` is
hardcoded inside the command), so there is deliberately no RW mapping path:
images carry no `.data`/`.bss`, and writable state lives in a
loader-allocated Normal/RW **persist block** (8 KB for the launcher,
`limits.persist_bytes` per app, default one page) handed to the image through
`ctx->data_base`/`ctx->data_size`.  The block is zero-filled once and stays
allocated while the image stays loaded, so state survives re-entries and app
launches.  A later `hbl-start` in the same SmileBASIC session recovers the
resident launcher's block from `launcher-state.json` in the run directory.

The map's own permission change publishes new code lines; `HBL_PUBLISH=1`
forces the per-64-byte patch path when rewriting code in place.

The launcher's code + data stay mapped for the whole session.  App images are
unmapped and their source blocks freed after the app returns.

## ABI

`struct hbx_ctx` (592 bytes, `sdk/abi.json`) is the only interface between the
host and an image:

* identity: magic `0x33425848`, ABI 3, size
* session: status/exit/frame, monotonic time, heap, data, code, stack
* services: control page, catalog, icons, assets, log ring
* `gpu`: borrowed display handles (instance, physical, device, queue,
  swapchain, images, views, format, dispatch table, vi window, surface)
* `input`: three pad entrypoints, touch entrypoint, scratch blocks
* `args[8]`: entry arguments (sequence, resume index, resume status, frame
  budget)

Touch input deserves a note: the block returned by
`nn::hid::GetTouchScreenState<16>` belongs to the system, not to us.  The SDK
treats it as bytes and reads the `nn::hid` layout (count at 0x10, entries at
0x18 with a 32-byte stride, x/y at +8/+12), falling back to the older 40-byte
stride with the count at 0x08 when the first read is implausible.  That way a
layout surprise degrades to "no touch points" only if both probes fail, and
never turns into a wild read.

`struct hbx_gpu` is filled by the **launcher** after it creates the display and
written into host memory, so the host sees it and passes it to apps.  Apps may
create their own Vulkan objects but never destroy the swapchain or its views.

The UI is exposed as a call-through table (`struct hbx_render`, `ctx->ui`):
function pointers that live in the launcher image.  Apps call `begin`, `rect`,
`text`, `line`, `circle`, `icon`, `vertices`, `present`.  This keeps renderer
internals private and versioned by a single `abi` word.

## Vulkan

The launcher is the display owner.  Bring-up follows the configuration already
known to produce a visible frame on this console:

1. `vkCreateInstance` — Vulkan 1.4, `VK_KHR_surface` + `VK_NN_vi_surface`
2. `vkCreateViSurfaceNN` on the host-created vi native window
3. physical device; a queue family with graphics **and** present support
4. `VkPhysicalDeviceDynamicRenderingFeatures{dynamicRendering}` (+ sync2 when
   present) → `vkCreateDevice` with `VK_KHR_swapchain`
5. 1920x1080 FIFO swapchain, RGBA8/BGRA8 sRGB-nonlinear, `minImageCount+1`
   clamped, opaque composite; one image view per image; acquire/render
   semaphores
6. one graphics pipeline with dynamic rendering, straight-alpha blending,
   static viewport/scissor, 24-byte vertices, push-constant screen size
7. per frame: acquire → record → submit → present → `vkQueueWaitIdle`

Textures: a 1024x1024 R8 glyph atlas and a 96x96 RGBA array of app icons, both
uploaded through a staging buffer before the first frame.  The atlas pixels
come from the host (`assets/font_atlas.r8`) so the app image stays 47 KB; the
glyph metrics are compiled in.

Descriptor set: two combined image samplers.  Vertex `tex` word selects
`solid` (vertex colour), `glyph` (R8 alpha) or `icon` (array layer).

Frame budget: at 60 Hz the CPU rebuilds ~78,000 vertices per frame
(1.9 MB of vertex data), one draw call.  If geometry ever exceeded the 163,840
vertex buffer the renderer drops the excess and reports overflow; the UI test
asserts this never happens.

## Rendering model

The original HBLauncher composed its UI in a single full-screen fragment
shader.  This implementation keeps its equations and layout but evaluates them
on the CPU:

* wave crest `336 + 30 sin(.0027x + .72t) + 14 sin(.0053x − .91t)`
* three translucent sheets above the crest, 20 contour strands, 6 ribbons,
  8 mirrored echoes, 32 bubbles — each tessellated into triangles
* rounded rectangles as fans, strokes as rings, lines as feathered 3-band
  strips (the soft 1 px falloff replaces the shader's `smoothstep`)
* text as glyph quads from the baked atlas, pixel-snapped

Everything below the crest is paper white; the app grid starts at y = 551 with
three 1320x124 cards, 151 px apart, matching the reference layout.

## App lifecycle

```
launcher entry ──► user presses A ──► loading panel presented
      │                                   │
      │ control->launch = index + 1       │
      ▼                                   ▼
host reads control, places package: code RX-mapped, persist RW block, heap, stack, assets
      │
      ├─ native: call entry with ctx; app runs until B/exit or frame limit
      └─ compat: run the proven external loader for a finite session
      │
      ▼
host unmaps app images, frees blocks, sets HBX_FLAG_RESUME + args
      │
      ▼
launcher draws "Returned from <title>" toast and keeps running
```

The host refuses packages whose code/data ranges would overlap the resident
launcher, checks every hash from `build/report.json`, and logs each stage.

## Failure behaviour

* Display creation failure → `ctx->status = HBX_ERR_GPU`, host reports it and
  the last frame stays on screen.
* App launch failure → status propagates as `args[2]`; the launcher shows an
  "App unavailable" panel with the status.
* Present failure → status `HBX_ERR_GPU`; the host stops re-entering the
  launcher rather than looping.
* The display is retained by design (the proven policy never tears down the vi
  swapchain); reclaim it by restarting SmileBASIC.
