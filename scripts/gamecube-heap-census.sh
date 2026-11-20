#!/usr/bin/env bash
# Resolve the engine's heap census (RIDE_HEAP_CENSUS=1 / SWEEP_HEAP_CENSUS=1)
# into "live KiB  count  function  file:line" per call site. No AI tooling.
#
# Usage: scripts/gamecube-heap-census.sh <ride dir> [elf] [n]
#   elf defaults to OUT/bin/xash (must be the build the ride ran);
#   n picks the n-th "map active" census in the log (default: the last).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ride="${1:?ride dir}"
elf="${2:-$ROOT/OUT/bin/xash}"
pick="${3:-0}"
log="$ride/dolphin-user/Logs/dolphin.log"
A2L="${DEVKITPPC:-/opt/devkitpro/devkitPPC}/bin/powerpc-eabi-addr2line"
[[ -f "$log" ]] || { echo "no $log"; exit 1; }

block=$(awk -v pick="$pick" '
	/heap census at=/ { if (inb) blocks[n] = cur; n++; cur = $0 "\n"; inb = 1; next }
	inb && /heap census caller=/ { cur = cur $0 "\n"; next }
	inb { blocks[n] = cur; inb = 0 }
	END { if (inb) blocks[n] = cur; k = (pick > 0 && pick <= n) ? pick : n; printf "%s", blocks[k] }
' <(grep -a "heap census" "$log" | sed -E 's/.*OSREPORT\]: (\[[0-9:]+\] )?//'))
[[ -n "$block" ]] || { echo "no heap census in $log (was RIDE_HEAP_CENSUS=1 set?)"; exit 1; }

echo "$block" | head -1
echo "$block" | grep -o 'caller=0x[0-9a-f]* live=[0-9]* count=[0-9]*' | while read -r c l n; do
	addr=${c#caller=}
	where=$("$A2L" -f -C -e "$elf" "$addr" 2>/dev/null | paste -sd' ' | sed "s|$ROOT/||; s|build/../||")
	awk -v b="${l#live=}" -v n="${n#count=}" -v w="$where" 'BEGIN { printf "%8.1f KiB %6s  %s\n", b / 1024, n, w }'
done
