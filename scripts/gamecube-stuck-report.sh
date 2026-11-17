#!/usr/bin/env bash
# Classify "stuck" player samples in GameCube ride logs (no AI tooling).
#
# The engine's ride trace (sv_phys.c) logs player origin and
# SV_TestEntityPosition every 64 frames.  The campaign autopilot
# (sv_gc_campaign.c) moves the player by setting origin directly, without
# collision, so a stuck sample is only evidence of a game bug if it happens
# before the autopilot touched the player on that map.
#
# For each map visit:   start  = forced start map (spawned at info_player_start)
#                       arrive = reached by a level change
# and each stuck sample is labelled
#   pre-autopilot   before this map's CAMPAIGN walk-in (spawn/landmark result)
#   post-autopilot  after the walk-in (autopilot artifact unless shown otherwise)
#
# Usage: scripts/gamecube-stuck-report.sh <ride dir or sweep dir> [...]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

logs=()
for a in "$@"; do
	if [[ -f "$a/sweep.csv" ]]; then
		while read -r r; do logs+=("$r/dolphin-user/Logs/dolphin.log"); done \
			< <(tail -n +2 "$a/sweep.csv" | grep -o '\.ai/logs/ride-[^,]*')
	else
		logs+=("$a/dolphin-user/Logs/dolphin.log")
	fi
done

for log in "${logs[@]}"; do
	[[ -f "$log" ]] || continue
	grep -a -o -E 'CAMPAIGN map [a-z0-9_]+ at t=[0-9.]+s \([^)]*\)|CAMPAIGN walk-in [a-z0-9_]+ ->|CAMPAIGN FORCED changelevel [a-z0-9_]+ ->|ride map=[a-z0-9_]+ player=\([^)]*\) stuck=[01] solid=[0-9]+ hull=[^ ]+ vel=[0-9]+ flags=0x[0-9a-f]+ movetype=[0-9]+ ground=-?[0-9]+' "$log" \
	| awk -v ride="$(basename "$(dirname "$(dirname "$(dirname "$log")")")")" '
		/^CAMPAIGN map / { map = $3; how = ($0 ~ /from start/) ? "start" : "arrive"; moved = 0; next }
		/^CAMPAIGN walk-in / || /^CAMPAIGN FORCED/ { moved = 1; next }
		/^ride map=/ {
			split($2, m, "="); if (m[2] != map) next
			if ($4 != "stuck=1") next
			key = map " " how " " (moved ? "post-autopilot" : "pre-autopilot") " " $3 " " $8 " " $9
			n[key]++
		}
		END { for (k in n) printf "%-28s %3d  %s\n", ride, n[k], k }'
done | sort -k3,3 -k5,5
