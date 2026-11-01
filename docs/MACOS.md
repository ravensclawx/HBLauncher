# HBLauncher on macOS

The build, the offline tests and the mock session run on macOS (tested on Apple Silicon, macOS 27,
devkitA64 r30 / GCC 16.1). Nothing here has been tried against a console from a Mac.

## Prerequisites

| need | macOS |
| --- | --- |
| AArch64 cross GCC | devkitPro `devkitA64` (`sudo dkp-pacman -S switch-dev`), picked up by `tools/cross.sh` |
| Vulkan headers for the target | `brew install vulkan-headers` (`tools/target-cflags.sh` adds only that folder) |
| Node 18+, Python 3, glslang, spirv-tools | Homebrew |
| Pillow + DejaVu fonts | optional: only to regenerate the committed font atlas |

`HBL_CROSS` (toolchain prefix), `HBL_TARGET_CFLAGS`, `HOST_CC` and `HBL_HOST_CFLAGS` override the detection.

## Build and test

```sh
sh tools/build-all.sh
sh tests/run.sh
./hbl-start --mock
```

What macOS cannot run: the qemu-aarch64 tests (no qemu-user on macOS; the binaries are still built) and
the Vulkan preview/UI render (no software Vulkan ICD by default; set `VK_ICD_FILENAMES` to try MoltenVK).
On an AArch64 host the SDK runtime test runs the AArch64 code paths natively (`HBX_TEST_NO_SLEEP`).

## Console link

Same wired link as on Linux, set with `ifconfig` instead of `ip`:

```sh
sudo ifconfig <iface> inet 10.42.0.1 netmask 255.255.255.0 alias
```

`networksetup -listallhardwareports` shows which `enX` is the USB/Thunderbolt Ethernet adapter.
`./hbl-start --preflight` reports the link, and lists capture cards from `system_profiler` (they appear as
cameras on macOS, not `/dev/video*`).

The exploit runner (`ns2jb-kurumuz`, `HBL_NS2JB`) is a separate checkout and is not covered here.
