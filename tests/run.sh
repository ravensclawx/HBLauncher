#!/bin/sh
# Full offline verification.  No console required.
set -e
cd "$(dirname "$0")/.."
NODE=$(command -v node || true)
[ -n "$NODE" ] || NODE=${HBL_NODE:-"$PWD/../tools/node"}
if [ ! -x "$NODE" ] && ! command -v "$NODE" >/dev/null 2>&1; then
  echo "tests/run.sh: node not found; set HBL_NODE" >&2; exit 2
fi

echo "== generated assets are current =="
if python3 -c "import PIL" 2>/dev/null; then
  python3 tools/make_font_atlas.py --check
else
  echo "(font atlas check skipped: Pillow not installed)"
fi
python3 tools/build_shaders.py --check
python3 tools/gen_abi.py --check

echo "== SDK runtime (host + qemu-aarch64) =="
sh tests/abi/build.sh

echo "== hardware probe (mock backend) =="
"$NODE" host/probe.cjs --mock

echo "== host lifecycle =="
"$NODE" tests/host/lifecycle.test.cjs

echo "== launcher navigation logic =="
mkdir -p build/tests
# AArch64 hosts: keep hbx_app.c off its console thread path (see tests/abi/build.sh).
case "$(uname -m)" in arm64|aarch64) LOGIC_DEFS=-DHBX_TEST_NO_SLEEP ;; *) LOGIC_DEFS= ;; esac
${HOST_CC:-gcc} -std=gnu11 -O2 -D_GNU_SOURCE $LOGIC_DEFS -Wall -Wextra -Werror \
  -Isdk/include -Isdk/src -Ilauncher/src $(sh tools/target-cflags.sh) \
  tests/ui/logic.test.c launcher/src/launcher_logic.c sdk/src/hbx_app.c \
  -o build/tests/logic_test
./build/tests/logic_test

echo "== UI geometry + real Vulkan render =="
if [ "$(uname -s)" = Darwin ] && [ -z "${VK_ICD_FILENAMES:-}" ]; then
  echo "(skipped on macOS: needs a host Vulkan ICD; set VK_ICD_FILENAMES, e.g. to MoltenVK)"
else
  sh tests/preview/build.sh
  "$NODE" tests/ui/ui.test.cjs
fi

echo "all offline tests passed"
