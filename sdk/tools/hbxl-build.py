#!/usr/bin/env python3
"""hbxl-build: build a Linux-personality app (static aarch64-linux-musl C/C++)
into an HBLauncher native package (code.bin, data.bin, report.json).

The app links against the toolchain in $HBXL_TOOLCHAIN (default
../tp-port/toolchain), whose musl routes syscalls through __hbx_syscall; the
runtime in sdk/linux implements them on the console.

Manifest keys used: id, kind ("native"), abi, sources, "linux": {
  "cflags": [...], "cxxflags": [...], "ldflags": [...], "libs": [...],
  "objects": [...prebuilt .o/.a relative to the app...],
  "code_base": "0x...", "data_base": "0x...",
  "fs_port": 39502 }            (optional: this app's own hbxl-fileserver)
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SDK = ROOT / "sdk"
TOOLCHAIN = Path(os.environ.get("HBXL_TOOLCHAIN", ROOT.parent / "tp-port" / "toolchain"))
PREFIX = TOOLCHAIN / "bin" / "aarch64-linux-musl-"
COMMON = ["-O2", "-g0", "-mbranch-protection=bti", "-fno-pie", "-fno-pic",
          "-ffunction-sections", "-fdata-sections", "-fno-plt", "-D_GNU_SOURCE"]
RUNTIME = ["start.S", "runtime.c", "memory.c", "thread.c", "vfs.c"]


def fail(message):
    print("hbxl-build: " + message, file=sys.stderr)
    sys.exit(1)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(" ".join(map(str, cmd)) + "\n" + r.stdout + r.stderr)
        fail("command failed")
    return r


def vulkan_include(out):
    """Vulkan headers are architecture-neutral; expose only them (the host's
    /usr/include also holds glibc headers that must not shadow musl's)."""
    inc = out / "vk-include"
    inc.mkdir(parents=True, exist_ok=True)
    for name in ("vulkan", "vk_video"):
        link = inc / name
        if not link.exists() and Path("/usr/include", name).exists():
            link.symlink_to(Path("/usr/include", name))
    return inc


def compile_one(source, obj, flags, cxx):
    tool = str(PREFIX) + ("g++" if cxx else "gcc")
    run([tool, *flags, "-c", str(source), "-o", str(obj)])


def segments(elf):
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        fail("not an ELF64 file")
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    loads = []
    for i in range(phnum):
        p_type, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + i * phentsize)
        if p_type == 1:
            loads.append((vaddr, data[offset:offset + filesz], memsz, flags))
    return loads


def symbol(nm_out, name):
    for line in nm_out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    fail(f"symbol {name} not found")


def build_runtime_lib(out_lib):
    """libhbxl.a: the runtime + SDK helpers, for projects linked by their own
    build system (CMake) with -nostartfiles and sdk/linux/hbxl.ld."""
    out = Path(out_lib).resolve().parent / "hbxl-obj"
    out.mkdir(parents=True, exist_ok=True)
    vk = vulkan_include(out)
    flags = COMMON + [f"-I{SDK / 'include'}", f"-I{SDK / 'linux'}", f"-idirafter{vk}"]
    objects = []
    for name in RUNTIME:
        obj = out / ("hbxl_" + name.replace(".", "_") + ".o")
        compile_one(SDK / "linux" / name, obj, flags + ["-Wall", "-Wextra", "-Werror", "-fno-builtin"], False)
        objects.append(obj)
    for name in ("hbx_app.c", "hbx_math.c"):
        obj = out / ("sdk_" + name.replace(".", "_") + ".o")
        compile_one(SDK / "src" / name, obj, flags, False)
        objects.append(obj)
    Path(out_lib).unlink(missing_ok=True)
    run([str(PREFIX) + "ar", "rcs", str(out_lib), *map(str, objects)])
    print(f"[hbxl-build] runtime library {out_lib}")


def pack(elf, app, code_base, data_base):
    """Split a linked image into the package files the host loader maps."""
    manifest = json.loads((app / "manifest.json").read_text())
    out = app / "build"
    out.mkdir(parents=True, exist_ok=True)
    loads = segments(elf)
    code = next((d for v, d, m, f in loads if v == code_base), None)
    data_seg = next(((d, m) for v, d, m, f in loads if v == data_base), (b"", 0))
    if code is None:
        fail("no code segment at the code base")
    if code_base + len(code) > data_base:
        fail(f"code ({len(code)} bytes) overlaps the data base; raise linux.data_base")
    data, memsz = data_seg
    nm = run([str(PREFIX) + "nm", str(elf)]).stdout
    entry = symbol(nm, "_start")
    spawn = symbol(nm, "_spawn")
    (out / "code.bin").write_bytes(code)
    (out / "data.bin").write_bytes(data)
    report = {
        "format": "hblauncher-native-app/1",
        "id": manifest["id"],
        "kind": manifest["kind"],
        "abi": manifest["abi"],
        "runtime": "linux",
        "codeBase": hex(code_base),
        "dataBase": hex(data_base),
        "entry": hex(entry),
        "spawn": hex(spawn),
        "codeBytes": len(code),
        "codePages": (len(code) + 0xFFF) // 0x1000,
        "dataBytes": len(data),
        "bssBytes": max(0, memsz - len(data)),
        "dataRegionBytes": memsz,
        "codeSha256": hashlib.sha256(code).hexdigest(),
        "dataSha256": hashlib.sha256(data).hexdigest(),
    }
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[hbxl-build] {manifest['id']}: code {len(code)} B, data {len(data)} B, "
          f"region {memsz} B, entry 0x{entry:x}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", nargs="?")
    parser.add_argument("--runtime-lib", help="write libhbxl.a and exit")
    parser.add_argument("--pack", help="package an already linked ELF into APP")
    args = parser.parse_args()
    if args.runtime_lib:
        build_runtime_lib(args.runtime_lib)
        return
    app = Path(args.app).resolve()
    if args.pack:
        linux = json.loads((app / "manifest.json").read_text()).get("linux", {})
        pack(Path(args.pack), app, int(linux.get("code_base", "0x0c000000"), 16),
             int(linux.get("data_base", "0x0d000000"), 16))
        return
    manifest = json.loads((app / "manifest.json").read_text())
    linux = manifest.get("linux", {})
    code_base = int(linux.get("code_base", "0x0c000000"), 16)
    data_base = int(linux.get("data_base", "0x0d000000"), 16)
    out = app / "build"
    obj_dir = out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    if not (TOOLCHAIN / "bin").exists():
        fail(f"toolchain not found at {TOOLCHAIN}")

    vk = vulkan_include(out)
    includes = [f"-I{SDK / 'include'}", f"-I{SDK / 'linux'}", f"-I{app / 'src'}", f"-idirafter{vk}"]
    cflags = COMMON + includes + linux.get("cflags", [])
    cxxflags = COMMON + includes + ["-std=gnu++20"] + linux.get("cxxflags", [])

    objects = []
    for name in RUNTIME:
        obj = obj_dir / ("hbxl_" + name.replace(".", "_") + ".o")
        compile_one(SDK / "linux" / name, obj, cflags + ["-Wall", "-Wextra", "-Werror", "-fno-builtin"], False)
        objects.append(obj)
    for name in ("hbx_app.c", "hbx_math.c"):            # SDK helpers: input, pacing, log
        obj = obj_dir / ("sdk_" + name.replace(".", "_") + ".o")
        compile_one(SDK / "src" / name, obj, cflags, False)
        objects.append(obj)
    for rel in manifest.get("sources", []):
        source = app / rel
        obj = obj_dir / (rel.replace("/", "_").replace(".", "_") + ".o")
        cxx = source.suffix in (".cc", ".cpp", ".cxx")
        compile_one(source, obj, cxxflags if cxx else cflags, cxx)
        objects.append(obj)
    objects += [app / o for o in linux.get("objects", [])]

    elf = out / "app.elf"
    run([str(PREFIX) + "g++", "-static", "-nostartfiles", "-no-pie",
         f"-Wl,-T,{SDK / 'linux' / 'hbxl.ld'}",
         f"-Wl,--defsym=HBXL_CODE_BASE=0x{code_base:x}", f"-Wl,--defsym=HBXL_DATA_BASE=0x{data_base:x}",
         *([f"-Wl,--defsym=HBXL_FS_PORT={int(linux['fs_port'])}"] if "fs_port" in linux else []),
         "-Wl,--gc-sections", "-Wl,-z,noexecstack", "-Wl,--build-id=none", "-Wl,--eh-frame-hdr",
         *map(str, objects), *linux.get("ldflags", []), *linux.get("libs", []),
         "-o", str(elf)])

    loads = segments(elf)
    code = next((d for v, d, m, f in loads if v == code_base), None)
    data_seg = next(((d, m) for v, d, m, f in loads if v == data_base), (b"", 0))
    if code is None:
        fail("no code segment at the code base")
    if code_base + len(code) > data_base:
        fail(f"code ({len(code)} bytes) overlaps the data base; raise linux.data_base")
    data, memsz = data_seg
    nm = run([str(PREFIX) + "nm", str(elf)]).stdout
    entry = symbol(nm, "_start")
    spawn = symbol(nm, "_spawn")
    (out / "code.bin").write_bytes(code)
    (out / "data.bin").write_bytes(data)
    report = {
        "format": "hblauncher-native-app/1",
        "id": manifest["id"],
        "kind": manifest["kind"],
        "abi": manifest["abi"],
        "runtime": "linux",
        "codeBase": hex(code_base),
        "dataBase": hex(data_base),
        "entry": hex(entry),
        "spawn": hex(spawn),
        "codeBytes": len(code),
        "codePages": (len(code) + 0xFFF) // 0x1000,
        "dataBytes": len(data),
        "bssBytes": max(0, memsz - len(data)),
        "dataRegionBytes": memsz,
        "codeSha256": hashlib.sha256(code).hexdigest(),
        "dataSha256": hashlib.sha256(data).hexdigest(),
    }
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[hbxl-build] {manifest['id']}: code {len(code)} B, data {len(data)} B, "
          f"region {memsz} B, entry 0x{entry:x}")


if __name__ == "__main__":
    main()
