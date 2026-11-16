#!/usr/bin/env bash
# Compact, LLM-free digest of a chapter sweep (scripts/gamecube-chapter-sweep.sh).
# Prints at most ~30 lines so a person (or an agent) can read the outcome
# without opening the per-run logs.  Pure shell/awk; no AI tooling.
#
# Usage: scripts/gamecube-sweep-digest.sh [.ai/sweeps/<stamp>]   (default: newest)
#        scripts/gamecube-sweep-digest.sh --status                 one-line progress
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

if [[ "${1:-}" == "--status" ]]; then
	dir=$(find .ai/sweeps -maxdepth 1 -type d -name '20*' | sort | tail -1)
	[[ -n "$dir" ]] || { echo "no sweep"; exit 0; }
	rows=$(( $(wc -l < "$dir/sweep.csv") - 1 ))
	if pgrep -f gamecube-chapter-sweep.sh >/dev/null; then state=running; else state=stopped; fi
	[[ -f "$dir/summary.md" ]] && state=finished
	echo "$dir: $state, $rows runs recorded, last: $(tail -1 "$dir/sweep.csv" | cut -d, -f1,2,4,5,13)"
	exit 0
fi

dir="${1:-$(find .ai/sweeps -maxdepth 1 -type d -name '20*' | sort | tail -1)}"
csv="$dir/sweep.csv"
[[ -f "$csv" ]] || { echo "no $csv"; exit 1; }

echo "== $dir ($(sed -n 2p "$csv" | cut -d, -f3))"
awk -F, 'NR>1 {
	n++; if ($5!="no") fatal++; if ($13=="yes") freeze++
	if ($4 ~ /^"?[^" ]+ [^" ]+/) moved++
	if ($11+0>0) alloc++
	if ($5 ~ /failed|skipped/) bad[$5]++
} END {
	printf "runs=%d advanced_past_start=%d fatal_or_failed=%d freeze=%d with_alloc_failures=%d\n", n, moved, fatal, freeze, alloc
	for (k in bad) printf "  %s: %d\n", k, bad[k]
}' "$csv"

# Where each run ended: the last guest OSREPORT line, timestamps stripped,
# grouped so a shared hang shows up as one line with a count.
echo "== last guest line per run (grouped)"
tail -n +2 "$csv" | while IFS=, read -r map run _ _ _ _ _ _ _ _ _ _ freeze _; do
	ride=$(grep -o '\.ai/logs/ride-[^,]*' <<<"$(grep "^$map,$run," "$csv")" | head -1)
	log="$ride/dolphin-user/Logs/dolphin.log"
	last=$( [[ -f "$log" ]] && grep 'OSREPORT' "$log" | tail -1 | sed -E 's/.*OSREPORT\]: (\[[0-9:]+\] )?//' | cut -c1-110 )
	echo "${last:-<no guest log>}|$map-$run"
done | awk -F'|' '{c[$1]++; r[$1]=r[$1] " " $2} END {for (k in c) printf "%3d  %s  [%s ]\n", c[k], k, (length(r[k])>60 ? substr(r[k],1,60) " ..." : r[k])}' | sort -rn | head -20

echo "== distinct allocation failures"
tail -n +2 "$csv" | grep -o '\.ai/logs/ride-[^,]*' | sort -u | while read -r ride; do
	grep -h -o "can't alloc [0-9]* bytes[^\"]*" "$ride/dolphin-user/Logs/dolphin.log" 2>/dev/null
done | sed -E 's/[0-9]+ bytes/N bytes/' | sort | uniq -c | sort -rn | head -5
