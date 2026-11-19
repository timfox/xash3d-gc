#!/usr/bin/env bash
# Boot Material Lab in Flatpak Dolphin (GUI+OpenGL) and dump PNG frames.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOL_SRC="$ROOT/gx-tev-material-lab.dol"
OUT_DIR="${1:-/tmp/gx-tev-material-lab-cap}"
TIMEOUT_SEC="${DOLPHIN_TIMEOUT:-18}"

if [[ ! -f "$DOL_SRC" ]]; then
	echo "error: missing $DOL_SRC (run make first)" >&2
	exit 1
fi

cp -f "$DOL_SRC" /tmp/gx-tev-material-lab.dol
DOL=/tmp/gx-tev-material-lab.dol

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/user/Config" "$OUT_DIR/frames" "$OUT_DIR/shots"

cat > "$OUT_DIR/user/Config/Dolphin.ini" <<EOF
[Core]
CPUThread = True
MMU = False
[DSP]
Backend = Null
[Movie]
DumpFrames = True
DumpFramesSilent = True
[General]
DumpPath = $OUT_DIR/frames
[Interface]
ConfirmOnStop = False
PauseOnPanic = False
ShowDebuggingWarning = False
ShowPanicHandlers = False
[Logger]
Verbosity = 3
WriteToFile = True
WriteToConsole = True
[Logs]
OSREPORT = True
OSREPORT_HLE = True
BOOT = True
CORE = True
EOF

cat > "$OUT_DIR/user/Config/GFX.ini" <<EOF
[Settings]
DumpFramesAsImages = True
PNGCompressionLevel = 1
ShowFPS = False
[Hacks]
XFBToTextureEnable = False
DisableCopyToVRAM = False
SkipDuplicateXFBs = False
ImmediateXFBEnable = False
EOF

cat > "$OUT_DIR/user/Config/Logger.ini" <<EOF
[Logs]
BOOT = True
CORE = True
OSREPORT = True
OSREPORT_HLE = True
[Options]
Verbosity = 3
WriteToConsole = True
WriteToFile = True
EOF

pkill -f 'org.DolphinEmu.dolphin-emu' 2>/dev/null || true
pkill -f dolphin-emu 2>/dev/null || true
sleep 1

echo "==> Flatpak Dolphin OpenGL dump (${TIMEOUT_SEC}s)"
flatpak run --filesystem=/tmp:rw --filesystem=home --nosocket=wayland --socket=x11 --device=dri \
	org.DolphinEmu.dolphin-emu \
	-e "$DOL" -u "$OUT_DIR/user" -v OpenGL \
	>"$OUT_DIR/out.txt" 2>"$OUT_DIR/err.txt" &
DPID=$!

# Wait for frames, then stop
for i in $(seq 1 "$TIMEOUT_SEC"); do
	sleep 1
	n=$(find "$OUT_DIR/frames" -name 'framedump_*.png' 2>/dev/null | wc -l)
	echo "  t=${i}s frames=${n}"
	if (( n > 90 )); then
		break
	fi
done

kill "$DPID" 2>/dev/null || true
sleep 1
kill -9 "$DPID" 2>/dev/null || true
wait "$DPID" 2>/dev/null || true
pkill -f 'org.DolphinEmu.dolphin-emu' 2>/dev/null || true

mapfile -t FRAMES < <(find "$OUT_DIR/frames" -name 'framedump_*.png' | sort -V)
echo "==> captured ${#FRAMES[@]} frames"
if ((${#FRAMES[@]} == 0)); then
	echo "error: no frames" >&2
	tail -40 "$OUT_DIR/err.txt" || true
	exit 1
fi

# Stable mid-run frame
idx=$(( ${#FRAMES[@]} * 2 / 3 ))
cp -f "${FRAMES[$idx]}" "$OUT_DIR/shots/full-frame.png"
cp -f "${FRAMES[10]:-${FRAMES[0]}}" "$OUT_DIR/shots/early.png"
cp -f "${FRAMES[-1]}" "$OUT_DIR/shots/late.png"
echo "==> proof: $OUT_DIR/shots/full-frame.png"
ls -la "$OUT_DIR/shots"
