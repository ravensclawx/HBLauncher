#!/bin/sh
# Build the offline preview renderer (host Vulkan, lavapipe or any ICD).
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/preview tests/preview/out
gcc -std=gnu11 -O2 -D_GNU_SOURCE -Wall -Wextra \
  -Isdk/include -Isdk/src -Ilauncher/src \
  tests/preview/preview.c tests/preview/vk_loader.c \
  sdk/src/hbx_ui.c sdk/src/hbx_math.c sdk/src/hbx_ui_shaders.c \
  launcher/src/font_metrics.c launcher/src/launcher_ui.c \
  -o build/preview/hbl-preview -ldl
echo "built build/preview/hbl-preview"

# App previews: compile each shipped app's own entry point into a capture
# harness that uses the same renderer and the same atlas.
for app in hello-cpu gpu-triangle; do
  gcc -std=gnu11 -O2 -D_GNU_SOURCE -Wall -Wextra \
    -Isdk/include -Isdk/src -Ilauncher/src \
    tests/preview/app_preview.c tests/preview/vk_loader.c \
    sdk/src/hbx_ui.c sdk/src/hbx_math.c sdk/src/hbx_ui_shaders.c sdk/src/hbx_app.c \
    launcher/src/font_metrics.c "apps/$app/src/app.c" \
    -o "build/preview/hbl-app-$app" -ldl
  echo "built build/preview/hbl-app-$app"
done
