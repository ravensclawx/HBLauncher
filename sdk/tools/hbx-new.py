#!/usr/bin/env python3
"""Scaffold a new app from sdk/template.

Usage: hbx-new.py <apps/your-id> "Display Name" ["description"]
"""
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
TEMPLATE = ROOT / "sdk/template"


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    target = pathlib.Path(sys.argv[1]).resolve()
    name = sys.argv[2]
    description = sys.argv[3] if len(sys.argv) > 3 else "A new HBLauncher Native app"
    app_id = target.name
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,63}", app_id):
        print("app id must be lowercase [a-z0-9._-]", file=sys.stderr)
        return 2
    if target.exists() and any(target.iterdir()):
        print(f"{target} already exists and is not empty", file=sys.stderr)
        return 1
    for source in TEMPLATE.rglob("*"):
        if source.is_dir():
            continue
        relative = source.relative_to(TEMPLATE)
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        text = source.read_text()
        text = (text.replace("{{ID}}", app_id).replace("{{NAME}}", name)
                    .replace("{{DESCRIPTION}}", description))
        destination.write_text(text)
    (target / "assets").mkdir(exist_ok=True)
    print(f"created {target}")
    print(f"next: python3 sdk/tools/hbx-build.py {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
