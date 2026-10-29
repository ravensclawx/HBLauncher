# Linux personality (hbxl)

Runs ordinary static `aarch64-linux-musl` C/C++ programs as HBLauncher apps.

* **Toolchain**: musl-cross-make, GCC 15.1 + musl 1.2.6, with
  `musl-hbx-syscall-hooks.diff` (every syscall calls `__hbx_syscall`; the
  thread/clone assembly entry points are weak) and `-mbranch-protection=bti`
  for all target libraries (console code pages are BTI-guarded).  Config:
  `../tp-port/mcm/config.mak`; output `../tp-port/toolchain`.
* **Runtime** (`start.S`, `runtime.c`, `memory.c`, `thread.c`, `vfs.c`):
  mmap from Vulkan host-cached memory (SDK heap fallback), clone via nn::os
  threads pinned to cores, futex via WaitForAddress/SignalToAddress, files and
  stdout/stderr over TCP to `tools/hbxl-fileserver.py`, clocks from the system
  tick.  `_start(ctx)` saves the host frame, builds argv/envp/auxv with
  synthetic program headers (TLS, eh_frame_hdr) and calls `__libc_start_main`;
  `exit()` comes back to the host.
* **Build**: `python3 sdk/tools/hbxl-build.py apps/<app>` (manifest `"linux"`
  block for flags, prebuilt objects and code/data bases).
* **Run**: `python3 tools/hbxl-fileserver.py <root>` on the PC (10.42.0.1:39501).
