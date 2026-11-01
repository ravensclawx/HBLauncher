#!/bin/sh
# Build and run the SDK runtime tests on the host and under qemu-aarch64.
set -e
cd "$(dirname "$0")/../.."
OUT=build/tests
mkdir -p "$OUT"
CFLAGS="-std=gnu11 -O2 -Wall -Wextra -Werror -Isdk/include"
SOURCES="tests/abi/runtime_test.c tests/abi/harness.c sdk/src/hbx_app.c"

echo "== host =="
# On an AArch64 host (Apple Silicon, ARM Linux) hbx_app.c takes its console branch, which spawns
# nn::os threads into _start. Build it the way the qemu test does: synthetic tick, no console threads.
HOST_DEFS=
case "$(uname -m)" in arm64|aarch64) HOST_DEFS=-DHBX_TEST_NO_SLEEP ;; esac
${HOST_CC:-gcc} $CFLAGS $HOST_DEFS -o "$OUT/runtime_test_host" $SOURCES tests/abi/entry_host.c
"$OUT/runtime_test_host"

# The aarch64 tests are static Linux binaries run under qemu-user. Without qemu-aarch64 (macOS has no
# qemu-user) they are still cross-built, so compile/link problems show up, but not run.
CROSS=$(sh tools/cross.sh)
TARGET_CFLAGS=$(sh tools/target-cflags.sh)
QEMU=$(command -v qemu-aarch64 || true)
run_aarch64() {
  if [ -n "$QEMU" ]; then "$QEMU" "$1"; else echo "(built $1; not run: no qemu-aarch64 on this host)"; return 1; fi
}

echo "== qemu-aarch64 =="
${CROSS}gcc $TARGET_CFLAGS -std=gnu11 -O2 -ffreestanding -nostdlib -fno-builtin \
  -fno-pic -fno-pie -mcmodel=large -mbranch-protection=bti -march=armv8-a \
  -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -Wall -Wextra -Werror -DHBX_TEST_NO_SLEEP -Isdk/include \
  -Wl,-e,_start -static \
  -o "$OUT/runtime_test_aarch64" $SOURCES sdk/src/hbx_math.c tests/abi/entry_aarch64.S tests/abi/report_aarch64.c
run_aarch64 "$OUT/runtime_test_aarch64" && echo "qemu-aarch64 runtime tests passed"

echo "== entry trampoline (qemu-aarch64) =="
${CROSS}gcc $TARGET_CFLAGS -std=gnu11 -O2 -ffreestanding -nostdlib -fno-builtin \
  -fno-pic -fno-pie -mcmodel=large -mbranch-protection=bti -march=armv8-a \
  -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -Wall -Wextra -Werror -DHBX_TEST_NO_SLEEP -Isdk/include \
  -Wl,-e,_boot -static -o "$OUT/entry_test_aarch64" \
  tests/abi/entry_test.c tests/abi/entry_test.S sdk/src/crt0.S \
  sdk/src/hbx_app.c sdk/src/hbx_math.c tests/abi/entry_test_main.S
run_aarch64 "$OUT/entry_test_aarch64" && echo "entry trampoline test passed"
exit 0
