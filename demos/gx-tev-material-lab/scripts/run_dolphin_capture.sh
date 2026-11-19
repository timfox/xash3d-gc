#!/usr/bin/env bash
# Boot Material Lab in Flatpak Dolphin (GUI+OpenGL) and dump PNG frames.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOL_SRC="$ROOT/gx-tev-material-lab.dol"
OUT_DIR="${1:-/tmp/gx-tev-material-lab-cap}"
# Full reel ends ~frame 2420; DumpFrames is slower than realtime — default ~7 min.
TIMEOUT_SEC="${DOLPHIN_TIMEOUT:-400}"

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
	# Full reel through batch21 graphene/phase/orbital/biofilm (~2420 frames)
	if (( n > 2550 )); then
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

pick() {
	local i="$1" dest="$2"
	if (( i < ${#FRAMES[@]} )); then
		cp -f "${FRAMES[$i]}" "$dest"
	else
		echo "warn: pick $i past ${#FRAMES[@]} frames → using last for $(basename "$dest")" >&2
		cp -f "${FRAMES[-1]}" "$dest"
	fi
}
pick 70  "$OUT_DIR/shots/rust-pbr.png"
pick 115 "$OUT_DIR/shots/damascus.png"
pick 140 "$OUT_DIR/shots/corrugated.png"
pick 155 "$OUT_DIR/shots/solar.png"
pick 190 "$OUT_DIR/shots/rough-00.png"
pick 250 "$OUT_DIR/shots/rough-25.png"
pick 310 "$OUT_DIR/shots/rough-50.png"
pick 340 "$OUT_DIR/shots/rough-75.png"
pick 355 "$OUT_DIR/shots/rough-100.png"
pick 390 "$OUT_DIR/shots/indtex-bump.png"
pick 445 "$OUT_DIR/shots/hw-key.png"
pick 475 "$OUT_DIR/shots/hw-fill.png"
pick 505 "$OUT_DIR/shots/hw-rim.png"
pick 545 "$OUT_DIR/shots/stone-indtex.png"
pick 600 "$OUT_DIR/shots/emboss-lit.png"
pick 660 "$OUT_DIR/shots/hw-spec.png"
pick 700 "$OUT_DIR/shots/hw-spec-steel.png"
pick 760 "$OUT_DIR/shots/sky-noon-mirror.png"
pick 820 "$OUT_DIR/shots/sky-dusk-ibl.png"
pick 880 "$OUT_DIR/shots/sky-golden-ibl.png"
pick 960 "$OUT_DIR/shots/clearcoat.png"
pick 1020 "$OUT_DIR/shots/iridescent.png"
pick 1080 "$OUT_DIR/shots/sheen-cloth.png"
pick 1140 "$OUT_DIR/shots/sss-wrap.png"
pick 1200 "$OUT_DIR/shots/heat-warp.png"
pick 1280 "$OUT_DIR/shots/parallax-lite.png"
pick 1340 "$OUT_DIR/shots/car-paint.png"
pick 1410 "$OUT_DIR/shots/crt-scan.png"
pick 1490 "$OUT_DIR/shots/wet-surface.png"
pick 1560 "$OUT_DIR/shots/oxide-patina.png"
pick 1630 "$OUT_DIR/shots/porcelain.png"
pick 1700 "$OUT_DIR/shots/gold-leaf.png"
pick 1770 "$OUT_DIR/shots/frost-ice.png"
pick 1840 "$OUT_DIR/shots/caustics.png"
pick 1910 "$OUT_DIR/shots/velvet.png"
pick 1980 "$OUT_DIR/shots/carbon.png"
pick 2050 "$OUT_DIR/shots/pearl.png"
pick 2120 "$OUT_DIR/shots/graphene-foam.png"
pick 2190 "$OUT_DIR/shots/phase-glass.png"
pick 2260 "$OUT_DIR/shots/orbital-carbon.png"
pick 2330 "$OUT_DIR/shots/biofilm-sheen.png"
pick 2390 "$OUT_DIR/shots/rust-hero-end.png"
echo "==> proof: $OUT_DIR/shots/full-frame.png"
ls -la "$OUT_DIR/shots"
