#!/bin/sh
# Build the freestanding launcher image.
# Outputs build/launcher.elf, build/code.bin, build/data.bin, build/report.json
set -e
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
CC=${CC:-aarch64-linux-gnu-gcc}
OBJCOPY=${OBJCOPY:-aarch64-linux-gnu-objcopy}
NM=${NM:-aarch64-linux-gnu-nm}
SIZE=${SIZE:-aarch64-linux-gnu-size}
OUT=${1:-build}
mkdir -p "$OUT"
CFLAGS="-std=gnu11 -O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -fno-pie \
 -mcmodel=large -mbranch-protection=bti -march=armv8-a -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-common \
 -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
 -I$ROOT/sdk/include -I$ROOT/sdk/src -I$ROOT/launcher/src"

SOURCES="$ROOT/launcher/src/launcher_main.c $ROOT/launcher/src/launcher_ui.c \
 $ROOT/launcher/src/launcher_logic.c \
 $ROOT/launcher/src/launcher_vk.c $ROOT/sdk/src/hbx_app.c $ROOT/sdk/src/hbx_math.c \
 $ROOT/sdk/src/hbx_ui.c $ROOT/launcher/src/font_metrics.c $ROOT/sdk/src/hbx_ui_shaders.c"

echo "[launcher] compiling"
for source in $SOURCES; do
  object="$OUT/$(basename "$source" .c).o"
  $CC $CFLAGS -c "$source" -o "$object"
done
$CC $CFLAGS -c "$ROOT/sdk/src/crt0.S" -o "$OUT/crt0.o"

echo "[launcher] linking at 0x08000000"
$CC $CFLAGS -no-pie -nostdlib -Wl,-T,"$ROOT/launcher/link.ld" -Wl,--gc-sections \
  -Wl,--build-id=none -o "$OUT/launcher.elf" "$OUT/crt0.o" \
  "$OUT/launcher_main.o" "$OUT/launcher_ui.o" "$OUT/launcher_logic.o" "$OUT/launcher_vk.o" \
  "$OUT/hbx_app.o" "$OUT/hbx_math.o" "$OUT/hbx_ui.o" "$OUT/font_metrics.o" \
  "$OUT/hbx_ui_shaders.o" -lgcc

$OBJCOPY -O binary -j .text -j .rodata "$OUT/launcher.elf" "$OUT/code.bin"
$OBJCOPY -O binary -j .data "$OUT/launcher.elf" "$OUT/data.bin"

code_bytes=$(stat -c%s "$OUT/code.bin")
data_bytes=$(stat -c%s "$OUT/data.bin")
echo "[launcher] code.bin $code_bytes bytes, data.bin $data_bytes bytes"
$NM "$OUT/launcher.elf" | grep -E ' _start$| hbx_app_main$| hbx_launcher_draw$| hbx_ui_init$| launcher_display_create$' || true
$SIZE "$OUT/launcher.elf"

python3 - "$OUT" <<'PY'
import json, sys, subprocess, hashlib, pathlib
out = pathlib.Path(sys.argv[1])
elf = out / "launcher.elf"
symbols = {}
for line in subprocess.run(["aarch64-linux-gnu-nm", str(elf)], capture_output=True, text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3:
        symbols[parts[2]] = int(parts[0], 16)
code = (out / "code.bin").read_bytes()
data = (out / "data.bin").read_bytes()
report = {
    "format": "hblauncher-native-app/1",
    "kind": "launcher",
    "codeBase": "0x08000000",
    "dataBase": "0x0A000000",
    "codeBytes": len(code),
    "dataBytes": len(data),
    "bssStart": hex(symbols.get("__bss_start", 0)),
    "bssEnd": hex(symbols.get("__bss_end", 0)),
    "entry": hex(symbols.get("_start", 0)),
    "codeSha256": hashlib.sha256(code).hexdigest(),
    "dataSha256": hashlib.sha256(data).hexdigest(),
    "abi": 3,
    "symbols": {k: hex(v) for k, v in symbols.items() if k in
                ("_start", "hbx_app_main", "hbx_launcher_draw", "hbx_ui_init",
                 "launcher_display_create", "hbx_ui_present", "__bss_start", "__bss_end")},
}
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
PY
