#!/usr/bin/env bash
# Interactive Material Lab in Flatpak Dolphin (GUI). You stay in control.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOL_SRC="$ROOT/gx-tev-material-lab.dol"

if [[ ! -f "$DOL_SRC" ]]; then
	echo "error: missing $DOL_SRC (run make first)" >&2
	exit 1
fi

# Flatpak cannot see /run/media Expansion drives — stage into /tmp.
cp -f "$DOL_SRC" /tmp/gx-tev-material-lab.dol

exec flatpak run --filesystem=/tmp:rw --filesystem=home \
	--nosocket=wayland --socket=x11 --device=dri \
	org.DolphinEmu.dolphin-emu /tmp/gx-tev-material-lab.dol
