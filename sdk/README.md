# HBLauncher Native SDK

| | |
| --- | --- |
| `include/hbx_sdk.h` | ABI v3: context, GPU handoff, catalog, assets, log |
| `include/hbx_app.h` | runtime: input/touch, heap, time, pacing, assets, logging |
| `include/hbx_ui.h` | 2D UI call-through table (rect, text, line, icon, present) |
| `include/hbx_vk.h` | Vulkan 1.4 dispatch table, vi surface types, helpers |
| `src/` | freestanding runtime (app, math, renderer, shaders, crt0) |
| `tools/hbx-build.py` | compile + link an app package |
| `tools/hbx-new.py` | scaffold a new app from `template/` |
| `template/` | the minimal app |
| `examples/raw-vulkan/` | app-owned pipeline, command buffers and present |
| `abi.json` | machine-readable ABI (generated, do not hand-edit) |

Full guide: [`../docs/SDK.md`](../docs/SDK.md).
