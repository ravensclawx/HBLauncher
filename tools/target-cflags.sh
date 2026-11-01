#!/bin/sh
# Extra flags for the AArch64 target compiler. HBL_TARGET_CFLAGS wins.
# Debian's aarch64-linux-gnu-gcc finds <vulkan/vulkan.h> in /usr/include; devkitA64 and Homebrew GCCs
# do not, so point them at the Vulkan-Headers package only (searched last, nothing else from the host).
if [ -n "${HBL_TARGET_CFLAGS:-}" ]; then
  echo "$HBL_TARGET_CFLAGS"
  exit 0
fi
CROSS=$(sh "$(dirname "$0")/cross.sh") || exit 1
if echo '#include <vulkan/vulkan.h>' | "${CROSS}gcc" -ffreestanding -E -x c - >/dev/null 2>&1; then
  exit 0
fi
for dir in /opt/homebrew/opt/vulkan-headers/include /usr/local/opt/vulkan-headers/include \
           "${VULKAN_SDK:-/nonexistent}/include"; do
  if [ -f "$dir/vulkan/vulkan.h" ]; then
    echo "-idirafter $dir"
    exit 0
  fi
done
echo "target-cflags.sh: <vulkan/vulkan.h> not found (brew install vulkan-headers, or set HBL_TARGET_CFLAGS)" >&2
exit 1
