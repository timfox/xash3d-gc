#!/usr/bin/env python3
"""Reject tracked game data and generated exports. Does not inspect ignored user files."""
import subprocess
from pathlib import PurePosixPath

EXPORT_ROOTS = ('.ai/', 'OUT/', 'output/', 'tmp/', 'exports/', 'staging/',
                'assets-local/', 'Half-Life/', 'Half-Life 2/', 'games_data/',
                'docs/notes/menu-shots/', 'Documentation/images/',
                'demos/gx-tev-material-lab/textures/')
ASSET_SUFFIXES = frozenset(('.bsp', '.wad', '.pak', '.pk3', '.vpk', '.vtf', '.vmt',
    '.mdl', '.spr', '.wav', '.mp3', '.ogg', '.flac', '.gcvid', '.gcpcm', '.gct',
    '.cmp', '.tpl', '.tga', '.bmp', '.iso', '.gcm', '.dol', '.mp4', '.mkv',
    '.avi', '.webm', '.mov', '.dem', '.sav'))

def violations(paths):
    return sorted(p for p in paths if p.startswith(EXPORT_ROOTS)
                  or PurePosixPath(p).suffix.lower() in ASSET_SUFFIXES
                  or p == 'scripts/gc_hitext.bin')

def main():
    paths = subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0')
    bad = violations(p for p in paths if p)
    if bad:
        print('BYOD policy: remove these content/export files from Git tracking:')
        print('\n'.join(bad))
        return 1
    print('BYOD policy: tracked tree contains no prohibited asset/export paths.')
    print('This is a path/format check, not a copyright or Git-history certification.')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
