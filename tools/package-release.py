#!/usr/bin/env python3
"""Package current public source without Git history or local output."""
import io
import re
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXCLUDED = {'.git', 'build', '__pycache__', 'node_modules', 'release',
            'reference', 'run', 'saves'}
PRIVATE = re.compile(rb'(?i)(?:/h[o]me/[^/\s]+/|b(?:sellers|selelrs))')


def main():
    names = subprocess.check_output(
        ['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT
    ).decode().split('\0')
    files = []
    for name in sorted(set(names) - {''}):
        path = ROOT / name
        parts = Path(name).parts
        if EXCLUDED.intersection(parts) or not path.is_file():
            continue
        if path.is_symlink():
            raise SystemExit(f'Refusing symlink: {name}')
        if name.startswith('tests/preview/out/') or (
            name.startswith('apps/') and 'assets' in parts
        ):
            continue
        if path.suffix in {'.o', '.elf', '.a', '.log', '.pyc', '.nes', '.z64',
                           '.nds', '.iso', '.wad', '.nsp', '.xci', '.keys'}:
            continue
        if name.startswith('apps/') and path.name in {'code.bin', 'data.bin'}:
            continue
        if path.name.startswith('.env') and path.name != '.env.example':
            continue
        if path.name.endswith('.local.json'):
            continue
        data = path.read_bytes()
        if PRIVATE.search(data) or PRIVATE.search(name.encode()):
            raise SystemExit(f'Private identifier or workstation path found: {name}')
        files.append((name, data, bool(path.stat().st_mode & 0o111)))
    output = ROOT / 'release/hblauncher-native-source.tar.gz'
    output.parent.mkdir(exist_ok=True)
    with tarfile.open(output, 'w:gz') as archive:
        for name, data, executable in files:
            info = tarfile.TarInfo('hblauncher-native/' + name)
            info.size = len(data)
            info.mode = 0o755 if executable else 0o644
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))
    print(f'Packaged {len(files)} files: release/{output.name}')


if __name__ == '__main__':
    main()
