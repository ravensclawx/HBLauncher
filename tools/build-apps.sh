#!/bin/sh
# Build every native SDK app package under apps/.  Compatibility packages
# (kind nes/sm64) have no sources; they are validated by the loader instead.
set -e
cd "$(dirname "$0")/.."
for app in apps/*/; do
  [ -f "$app/manifest.json" ] || continue
  kind=$(python3 -c "import json,sys;print(json.load(open('$app/manifest.json')).get('kind',''))")
  if [ "$kind" != "native" ]; then
    echo "[build-apps] skipping $app ($kind compatibility package)"
    continue
  fi
  if python3 -c "import json,sys;sys.exit(0 if 'linux' in json.load(open('$app/manifest.json')) else 1)"; then
    # Linux-personality app: needs the musl toolchain (sdk/linux/README.md).
    if [ -x "${HBXL_TOOLCHAIN:-$PWD/../tp-port/toolchain}/bin/aarch64-linux-musl-gcc" ]; then
      python3 sdk/tools/hbxl-build.py "$app"
    else
      echo "[build-apps] skipping $app (musl toolchain not installed)"
    fi
    continue
  fi
  python3 sdk/tools/hbx-build.py "$app" --code-base 0x0c000000 --data-base 0x0d000000
done
