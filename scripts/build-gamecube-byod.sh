#!/usr/bin/env bash
# Build with a user's own game installation; never supply or download retail data.
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo 'Usage: bash scripts/build-gamecube-byod.sh /path/to/Half-Life/valve [private-output.iso]' >&2
  exit 2
fi
if [[ ! -d "$1" || ( ! -f "$1/liblist.gam" && ! -f "$1/pak0.pak" ) ]]; then
  echo 'BYOD: provide your own game directory containing liblist.gam or pak0.pak.' >&2
  exit 2
fi
DATA_DIR=$(cd -- "$1" && pwd -P)
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
OUTPUT=${2:-$ROOT/OUT/my-private-game.iso}
cd "$ROOT"
XASH3D_GC_SKIP_DISC_BUILD=1 bash scripts/build-gamecube.sh
python3 scripts/build-gamecube-disc.py --data "$DATA_DIR" --no-auto-extra-gamedirs \
  --dol "${XASH_GC_OUT:-OUT}/bin/boot.dol" --output "$OUTPUT"
