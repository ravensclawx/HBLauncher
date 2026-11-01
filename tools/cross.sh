#!/bin/sh
# Prints the AArch64 bare-metal cross toolchain prefix (e.g. "aarch64-linux-gnu-").
# HBL_CROSS wins; otherwise the first of: aarch64-linux-gnu- (Debian/Ubuntu), devkitA64's
# aarch64-none-elf- (macOS/Windows via devkitPro), aarch64-elf- (Homebrew). The images are
# freestanding (-nostdlib), so any of these GCCs produces the same kind of code.
if [ -n "${HBL_CROSS:-}" ]; then
  echo "$HBL_CROSS"
  exit 0
fi
for prefix in aarch64-linux-gnu- aarch64-none-elf- aarch64-elf-; do
  if command -v "${prefix}gcc" >/dev/null 2>&1; then
    echo "$prefix"
    exit 0
  fi
done
DKA64="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-"
if [ -x "${DKA64}gcc" ]; then
  echo "$DKA64"
  exit 0
fi
echo "cross.sh: no AArch64 cross GCC (apt install gcc-aarch64-linux-gnu, or devkitPro devkitA64, or set HBL_CROSS)" >&2
exit 1
