#!/usr/bin/env bash
# gamecube-ride.sh: play a built disc in Dolphin for a long, recorded ride and
# summarise it.  The boot probe (dolphin-boot-probe.sh) stops as soon as its
# readiness gates pass; a ride keeps going until a target map loads or a time
# limit, so it can follow the tram, changelevels and chapters.
#
#   scripts/gamecube-ride.sh <disc.iso> <probe-log-dir> [until-map] [minutes]
#
# <probe-log-dir> is a dolphin-boot-probe.sh log directory: its dolphin-user
# Config is copied into a fresh profile, so the ride boots exactly like that
# probe (same route via the disc's gamecube.cfg).  Dolphin runs as this
# checkout's xash-gc-dolphin under XWayland with OpenGL, holds the host-wide
# probe lock, and is recorded (scripts/playtest-record.py, this instance only).
# Output: .ai/logs/ride-<stamp>/ with dolphin.log and summary.txt.
set -uo pipefail
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
iso="${1:?disc.iso}"; probe_dir="${2:?probe log dir}"; until_map="${3:-}"; minutes="${4:-20}"
[[ -f "$iso" ]] || { echo "no disc $iso"; exit 2; }
[[ -d "$probe_dir/dolphin-user/Config" ]] || { echo "no Config in $probe_dir"; exit 2; }
DOLPHIN_EXE="$ROOT/3rdparty/dolphin/build/Binaries/xash-gc-dolphin"
[[ -x "$DOLPHIN_EXE" ]] || DOLPHIN_EXE="$ROOT/3rdparty/dolphin/build/Binaries/dolphin-emu"

out=".ai/logs/ride-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out/dolphin-user"
cp -r "$probe_dir/dolphin-user/Config" "$out/dolphin-user/"
user="$ROOT/$out/dolphin-user"
log="$user/Logs/dolphin.log"

exec 9>/dev/shm/xash3d-gc-dolphin-probe.lock
echo "==> waiting for the Dolphin probe lock"
flock -w 7200 9 || { echo "lock timeout"; exit 2; }

export EGL_PLATFORM=x11 QT_QPA_PLATFORM=xcb
unset WAYLAND_DISPLAY
rec_pid=
if [[ "${RIDE_RECORD:-1}" == "1" ]] && command -v ffmpeg >/dev/null; then
	python3 scripts/playtest-record.py record --pid 0 --sink "xashride$$" \
		--route "${RIDE_ROUTE:-ride}" --disc "$iso" --logdir "$out" --userdir "$user" \
		--json-out "$out/playtest.txt" > "$out/playtest-record.log" 2>&1 &
	rec_pid=$!
fi
"$DOLPHIN_EXE" -u "$user" -l -e "$iso" -v OGL > "$out/stdout.log" 2> "$out/stderr.log" &
dpid=$!
echo "==> ride $out dolphin=$dpid until=${until_map:-time} ${minutes}m"
end=$(( $(date +%s) + minutes * 60 ))
while kill -0 "$dpid" 2>/dev/null && (( $(date +%s) < end )); do
	if [[ -n "$until_map" ]] && grep -aqs "Xash3D GameCube: map loaded ${until_map}\b" "$log"; then
		sleep "${RIDE_TAIL_SEC:-20}"
		break
	fi
	grep -aqs "fatal breadcrumb held" "$log" && break
	sleep 5
done
kill "$dpid" 2>/dev/null; sleep 3; kill -9 "$dpid" 2>/dev/null
[[ -n "$rec_pid" ]] && wait "$rec_pid" 2>/dev/null

{
	echo "disc=$iso commit=$(git rev-parse --short HEAD)"
	echo "maps: $(grep -aho 'Xash3D GameCube: map loaded [a-z0-9_]*' "$log" | awk '{print $NF}' | uniq | tr '\n' ' ')"
	echo "alloc_failures=$(grep -ac 'mem FAIL\|OOM allocating' "$log")"
	echo "fatal: $(grep -a -m1 -o 'fatal message=.*' "$log")"
	echo "titles=$(grep -aho 'env_message, firing ([^)]*)' "$log" | sort -u | wc -l)"
	echo "repeated_alerts=$(grep -ac 'previous alert repeated' "$log")"
	echo "last: $(tail -1 "$log" | cut -c60-200)"
	grep -a 'PLAYTEST\|mp4' "$out/playtest-record.log" 2>/dev/null | tail -1
} | tee "$out/summary.txt"
