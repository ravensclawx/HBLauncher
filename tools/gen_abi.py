#!/usr/bin/env python3
"""Regenerate sdk/abi.json from the C headers (host compiler)."""
import json
import os
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
source = ROOT / "tools/abi_dump.c"
binary = ROOT / "build/abi_dump"
binary.parent.mkdir(exist_ok=True)
# Vulkan headers: system path on Linux; Homebrew's vulkan-headers on macOS (not searched by default).
extra = shlex.split(os.environ.get("HBL_HOST_CFLAGS", ""))
for prefix in ("/opt/homebrew/include", "/usr/local/include"):
    if sys.platform == "darwin" and os.path.exists(os.path.join(prefix, "vulkan/vulkan.h")):
        extra.append(f"-I{prefix}")
subprocess.run([os.environ.get("HOST_CC", "gcc"), "-std=gnu11", "-O0", "-Wall", "-Werror",
                f"-I{ROOT / 'sdk/include'}", *extra, str(source), "-o", str(binary)], check=True)
output = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
data = json.loads(output)
target = ROOT / "sdk/abi.json"
if "--check" in sys.argv:
    if not target.exists() or json.loads(target.read_text()) != data:
        print("sdk/abi.json is stale; run tools/gen_abi.py", file=sys.stderr)
        raise SystemExit(1)
    print("sdk/abi.json up to date")
    raise SystemExit(0)
target.write_text(json.dumps(data, indent=2) + "\n")
print(f"wrote {target.relative_to(ROOT)}")
