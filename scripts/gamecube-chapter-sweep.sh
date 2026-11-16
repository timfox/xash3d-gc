#!/usr/bin/env bash
# Unattended Half-Life chapter load-test sweep on the retail New Game route.
#
# For every start map, N runs: build + boot probe (menu New Game with the
# test-only start map), then a Dolphin ride where the campaign autopilot walks
# the chapter's level changes. One CSV row per run plus a markdown summary.
# These are LOAD TESTS (autopilot, forced start map), never playthroughs.
#
# Usage: scripts/gamecube-chapter-sweep.sh [map ...]
# Env:
#   SWEEP_RUNS=2          runs per map
#   SWEEP_MINUTES=15      ride length per run
#   SWEEP_STALL_SEC=120   a run whose log stops growing this long is a freeze
#   SWEEP_AUTOCAMPAIGN=25 seconds per map before the autopilot walks on
#   SWEEP_RECORD=1        record video (last frame saved for crash screens)
#   SWEEP_GDB=0           1: attach gdb and dump PC/backtrace when a run stalls
#   SWEEP_OUT=.ai/sweeps/<stamp>
#   GC_PROBE_TAG=sweep    probe/ride dir suffix; also honours XASH_GC_OUT,
#                         XASH_GC_WAF_OUT, HLSDK_GAMECUBE_DESTDIR and
#                         XASH_GAMECUBE_BUILD_LOCK for a private build tree.
# No AI tooling is invoked; everything below is shell, grep and awk.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

DEFAULT_MAPS="c1a1 c1a2 c1a3 c1a4 c2a1 c2a2 c2a3 c2a4 c2a4d c2a5 c3a1 c3a2 c4a1 c4a2 c4a1a c4a3"
MAPS=( ${@:-$DEFAULT_MAPS} )
RUNS="${SWEEP_RUNS:-2}"
MINUTES="${SWEEP_MINUTES:-15}"
STALL="${SWEEP_STALL_SEC:-120}"
AUTOCAMPAIGN="${SWEEP_AUTOCAMPAIGN:-25}"
RECORD="${SWEEP_RECORD:-1}"
export GC_PROBE_TAG="${GC_PROBE_TAG:-sweep}"
export GC_BOOT_PROBE_TIMEOUT="${GC_BOOT_PROBE_TIMEOUT:-1800}"
OUT="${SWEEP_OUT:-.ai/sweeps/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
CSV="$OUT/sweep.csv"
MD="$OUT/summary.md"

# The engine build links a prebuilt hlsdk archive and never recompiles the
# game code, so rebuild it first or game-side fixes are silently missing.
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}" DEVKITPPC="${DEVKITPPC:-/opt/devkitpro/devkitPPC}"
if [[ "${SWEEP_HLSDK_BUILD:-1}" == "1" ]]; then
	scripts/hlsdk-gamecube-build.sh > "$OUT/hlsdk-build.log" 2>&1 \
		|| { echo "hlsdk build failed; see $OUT/hlsdk-build.log"; exit 1; }
fi

echo "start_map,run,commit,maps_reached,fatal,host_error,dropped_entities_max,dropped_lines,count_mismatches,canary_changes,alloc_failures,stalled_sec,freeze,reduced,ride_dir,last_frame" > "$CSV"

csvq() { printf '"%s"' "$(printf '%s' "$1" | tr '\n' ' ' | sed 's/"/'"'"'/g')"; }

for map in "${MAPS[@]}"; do
	for run in $(seq 1 "$RUNS"); do
		echo "==> $map run $run"
		if (( $(df -Pk . | awk 'NR==2 {print $4}') < 8 * 1024 * 1024 )); then
			echo "$map,$run,$(git rev-parse --short HEAD),,skipped,,,,,,,,,$(csvq "under 8 GB free disk"),," >> "$CSV"
			continue
		fi
		probe_log="$OUT/probe-$map-$run.log"
		touch "$OUT/.run-start"
		KEEP_ISO=1 XASH_GC_PROBE_STARTMAP="$map" XASH_GC_PROBE_AUTOCAMPAIGN="$AUTOCAMPAIGN" \
			DOLPHIN_PROBE_LOCK_WAIT=7200 DOLPHIN_RETAIL=1 DOLPHIN_MENU_NEWGAME=1 \
			DOLPHIN_RECORD=0 DOLPHIN_VIDEO=Null DOLPHIN_TIMEOUT=240 \
			scripts/dolphin-boot-probe.sh > "$probe_log" 2>&1
		probe_dir=$(find .ai/logs -maxdepth 1 -name "dolphin-probe-*-$GC_PROBE_TAG" -newer "$OUT/.run-start" 2>/dev/null | sort | tail -1)
		iso="$probe_dir/xash3d-gc.iso"
		if [[ -z "$probe_dir" || ! -f "$iso" ]]; then
			echo "$map,$run,$(git rev-parse --short HEAD),,probe-failed,,,,,,,,,$(csvq "no ISO; see $probe_log"),," >> "$CSV"
			continue
		fi
		rm -rf "$probe_dir/dolphin-user/Dump"
		RIDE_RECORD="$RECORD" RIDE_VIDEO=OGL RIDE_STALL_SEC="$STALL" RIDE_GDB="${SWEEP_GDB:-0}" \
			scripts/gamecube-ride.sh "$iso" "$probe_dir" "" "$MINUTES" > "$OUT/ride-$map-$run.log" 2>&1
		rm -f "$iso"
		ride=$(find .ai/logs -maxdepth 1 -name "ride-*-$GC_PROBE_TAG" -newer "$OUT/.run-start" 2>/dev/null | sort | tail -1)
		if [[ -z "$ride" ]]; then
			echo "$map,$run,$(git rev-parse --short HEAD),,ride-failed,,,,,,,,,$(csvq "no ride dir; see $OUT/ride-$map-$run.log"),," >> "$CSV"
			continue
		fi
		log="$ride/stderr.log"
		summary="$ride/summary.txt"

		maps=$(grep -aho 'Xash3D GameCube: map loaded [a-z0-9_]*' "$log" 2>/dev/null | awk '{print $NF}' | uniq | tr '\n' ' ')
		fatal=$(grep -a -m1 -o 'fatal message=.*' "$log" 2>/dev/null | cut -c15-140)
		host_error=$(grep -a -m1 -o 'Host_Error: .*' "$log" 2>/dev/null | cut -c1-140)
		dropped_max=$(grep -aho 'Ignored [0-9]* entities' "$log" 2>/dev/null | awk '{print $2}' | sort -n | tail -1)
		dropped_lines=$(grep -ac 'Ignored [0-9]* entities' "$log" 2>/dev/null)
		mismatches=$(grep -ac 'should be [0-9]*)' "$log" 2>/dev/null)
		canary=$(grep -ac 'WORLD CANARY CHANGED' "$log" 2>/dev/null)
		alloc=$(grep -a -o 'alloc_failures=[0-9]*' "$summary" 2>/dev/null | cut -d= -f2)
		stalled=$(grep -a -o 'stalled_sec=[0-9]*' "$summary" 2>/dev/null | cut -d= -f2)
		freeze=no; [[ "${stalled:-0}" -gt 0 ]] && freeze=yes
		reduced=$(grep -aho 'REDUCED [^\r]*' "$log" 2>/dev/null | sed 's/ arena_tail_left=.*//; s/ bytes=[^ ]*//' | sort -u | tr '\n' ';')
		frame=""
		if [[ "$RECORD" == 1 ]]; then
			# playtest.txt holds the recording's JSON sidecar path; the video sits beside it.
			sidecar=$(head -1 "$ride/playtest.txt" 2>/dev/null)
			mp4="${sidecar%.json}.mp4"
			if [[ -n "$mp4" && -f "$mp4" ]]; then
				frame="$OUT/last-$map-$run.png"
				ffmpeg -v error -y -sseof -3 -i "$mp4" -frames:v 1 "$frame" 2>/dev/null || frame=""
				rm -f "$mp4"	# the last frame is what a crash screen needs
			fi
		fi
		# libogc's exception screen (DSI/ISI/...) is a dark-red frame; a silent
		# log after a crash otherwise reads as a freeze.
		if [[ -z "$fatal" && -n "$frame" && -f "$frame" ]] && python3 -I - "$frame" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); w, h = im.size
px = [im.getpixel((x, y)) for x in (w * 30 // 640, w * 600 // 640) for y in range(h // 8, h * 7 // 8, 8)]
sys.exit(0 if sum(r > 100 and g < 30 and b < 30 for r, g, b in px) > 0.8 * len(px) else 1)
PY
		then
			fatal="exception screen (see last frame)"
			freeze=no
		fi
		{
			printf '%s,%s,%s,' "$map" "$run" "$(git rev-parse --short HEAD)"
			csvq "$maps"; printf ','
			[[ -n "$fatal" ]] && csvq "$fatal" || printf 'no'; printf ','
			csvq "$host_error"; printf ','
			printf '%s,%s,%s,%s,%s,%s,%s,' "${dropped_max:-0}" "${dropped_lines:-0}" "${mismatches:-0}" \
				"${canary:-0}" "${alloc:-}" "${stalled:-0}" "$freeze"
			csvq "$reduced"; printf ',%s,%s\n' "$ride" "$frame"
		} >> "$CSV"
	done
done

# Markdown summary: one row per run, then per-map every-time vs intermittent.
{
	echo "# Chapter load-test sweep ($(date '+%Y-%m-%d %H:%M'), commit $(git rev-parse --short HEAD))"
	echo
	echo "Load tests on the retail New Game route with a test start map and the campaign autopilot — not playthroughs."
	echo
	echo "| Start | Run | Maps reached | Fatal | Freeze | Dropped ents (max) | Mismatches | Canary | Alloc fails |"
	echo "|---|---|---|---|---|---|---|---|---|"
	python3 - "$CSV" <<'PY'
import csv, sys, collections
rows = list(csv.DictReader(open(sys.argv[1])))
for r in rows:
    fatal = r['fatal'] if r['fatal'] not in ('no', '') else ('host error' if r['host_error'] else 'no')
    print(f"| {r['start_map']} | {r['run']} | {r['maps_reached'].strip()} | {fatal} | {r['freeze']} ({r['stalled_sec']}s) | {r['dropped_entities_max']} | {r['count_mismatches']} | {r['canary_changes']} | {r['alloc_failures']} |")
print()
print("## Every time vs intermittent")
print()
by = collections.defaultdict(list)
for r in rows:
    by[r['start_map']].append(r)
for m, rs in by.items():
    bad = [r for r in rs if r['fatal'] not in ('no', '') or r['freeze'] == 'yes' or r['host_error']]
    kind = 'clean' if not bad else ('every time' if len(bad) == len(rs) else 'intermittent')
    print(f"- **{m}**: {kind} ({len(bad)}/{len(rs)} runs failed)")
print()
print("## REDUCED fallbacks")
print()
seen = set()
for r in rows:
    for item in filter(None, (x.strip() for x in r['reduced'].split(';'))):
        if item not in seen:
            seen.add(item); print(f"- {item}")
if not seen:
    print("- none")
PY
	echo
	echo "Raw rows: \`$CSV\`. Last video frames (crash screens) sit beside it as last-<map>-<run>.png."
} > "$MD"
echo "summary: $MD"
