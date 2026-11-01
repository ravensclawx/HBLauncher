#!/bin/sh
# Reproducible build of everything offline: assets, launcher, apps, preview.
set -e
cd "$(dirname "$0")/.."
echo "== assets =="
# The atlas is committed; regenerating it needs Pillow and the DejaVu fonts.
if python3 -c "import PIL" 2>/dev/null && python3 tools/make_font_atlas.py --check >/dev/null 2>&1; then
  python3 tools/make_font_atlas.py
else
  echo "[assets] Pillow or DejaVu fonts missing: keeping the committed font atlas"
fi
python3 tools/build_shaders.py
python3 tools/gen_abi.py
echo "== launcher =="
sh launcher/build.sh
echo "== apps =="
sh tools/build-apps.sh
echo "== probe =="
python3 sdk/tools/hbx-build.py tests/probe
echo "== SDK example =="
python3 sdk/tools/hbx-build.py sdk/examples/raw-vulkan
echo "== host preview =="
# The preview renders through a host Vulkan ICD (Mesa lavapipe on Linux); macOS has none by default.
if [ "$(uname -s)" = Darwin ] && [ -z "${VK_ICD_FILENAMES:-}" ]; then
  echo "[preview] skipped on macOS (set VK_ICD_FILENAMES to a Vulkan ICD, e.g. MoltenVK, to build it)"
else
  sh tests/preview/build.sh
fi
echo "build complete"
