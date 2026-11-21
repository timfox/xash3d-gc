#!/usr/bin/env bash
# Boot Material Lab in Flatpak Dolphin (GUI+OpenGL) and dump PNG frames.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOL_SRC="$ROOT/gx-tev-material-lab.dol"
OUT_DIR="${1:-/tmp/gx-tev-material-lab-cap}"
# Full reel ends ~frame 6340 (batch67 rhenium/willow/garnet/ash); DumpFrames slower than realtime.
TIMEOUT_SEC="${DOLPHIN_TIMEOUT:-2100}"

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

# Stop a prior Material Lab Flatpak dump only (never film / xash-gc-dolphin)
pkill -f 'org.DolphinEmu.dolphin-emu.*gx-tev-material-lab' 2>/dev/null || true
pkill -f 'dolphin-emu -e /tmp/gx-tev-material-lab.dol' 2>/dev/null || true
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
	# Dolphin nests under DumpPath/Frames/ on some builds
	n=$(find "$OUT_DIR/frames" -name 'framedump_*.png' 2>/dev/null | wc -l || true)
	echo "  t=${i}s frames=${n}"
	# Full reel through rhenium/willow/garnet/ash (~6340 frames)
	if (( n > 6340 )); then
		break
	fi
done

kill "$DPID" 2>/dev/null || true
sleep 1
kill -9 "$DPID" 2>/dev/null || true
wait "$DPID" 2>/dev/null || true
# Only stop our Flatpak Material Lab dump — do not touch xash-gc-dolphin / film
pkill -f 'org.DolphinEmu.dolphin-emu.*gx-tev-material-lab' 2>/dev/null || true
pkill -f 'dolphin-emu -e /tmp/gx-tev-material-lab.dol' 2>/dev/null || true

mapfile -t FRAMES < <(find "$OUT_DIR/frames" -name 'framedump_*.png' 2>/dev/null | sort -V || true)
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
pick 2120 "$OUT_DIR/shots/concrete.png"
pick 2190 "$OUT_DIR/shots/wood-plank.png"
pick 2260 "$OUT_DIR/shots/brick.png"
pick 2330 "$OUT_DIR/shots/ceramic-tile.png"
pick 2400 "$OUT_DIR/shots/oled-void.png"
pick 2470 "$OUT_DIR/shots/anodized-mg.png"
pick 2540 "$OUT_DIR/shots/frost-poly.png"
pick 2610 "$OUT_DIR/shots/holo-vinyl.png"
pick 2680 "$OUT_DIR/shots/tungsten-foam.png"
pick 2750 "$OUT_DIR/shots/algae-glass.png"
pick 2820 "$OUT_DIR/shots/obsidian-wool.png"
pick 2890 "$OUT_DIR/shots/topaz-dust.png"
pick 2960 "$OUT_DIR/shots/palladium-foil.png"
pick 3030 "$OUT_DIR/shots/mycelium-tile.png"
pick 3100 "$OUT_DIR/shots/sapphire-wool.png"
pick 3170 "$OUT_DIR/shots/malachite-dust.png"
pick 3240 "$OUT_DIR/shots/osmium-mesh.png"
pick 3310 "$OUT_DIR/shots/coral-foam.png"
pick 3380 "$OUT_DIR/shots/emerald-tile.png"
pick 3450 "$OUT_DIR/shots/ash-wool.png"
pick 3520 "$OUT_DIR/shots/niobium-foil.png"
pick 3590 "$OUT_DIR/shots/lichen-foam.png"
pick 3660 "$OUT_DIR/shots/amethyst-wool.png"
pick 3730 "$OUT_DIR/shots/basalt-dust.png"
pick 3800 "$OUT_DIR/shots/rhenium-mesh.png"
pick 3870 "$OUT_DIR/shots/kelp-tile.png"
pick 3940 "$OUT_DIR/shots/citrine-wool.png"
pick 4010 "$OUT_DIR/shots/slate-foam.png"
pick 4080 "$OUT_DIR/shots/tantalum-foil.png"
pick 4150 "$OUT_DIR/shots/seaweed-foam.png"
pick 4220 "$OUT_DIR/shots/tourmaline-wool.png"
pick 4290 "$OUT_DIR/shots/pumice-dust.png"
pick 4360 "$OUT_DIR/shots/molybdenum-mesh.png"
pick 4430 "$OUT_DIR/shots/fern-foam.png"
pick 4500 "$OUT_DIR/shots/spinel-tile.png"
pick 4570 "$OUT_DIR/shots/charcoal-wool.png"
pick 4640 "$OUT_DIR/shots/hafnium-foil.png"
pick 4710 "$OUT_DIR/shots/driftwood-tile.png"
pick 4780 "$OUT_DIR/shots/aquamarine-wool.png"
pick 4850 "$OUT_DIR/shots/scoria-dust.png"
pick 4920 "$OUT_DIR/shots/vanadium-foil.png"
pick 4990 "$OUT_DIR/shots/sedge-foam.png"
pick 5060 "$OUT_DIR/shots/peridot-wool.png"
pick 5130 "$OUT_DIR/shots/cinder-dust.png"
pick 5200 "$OUT_DIR/shots/zirconium-mesh.png"
pick 5270 "$OUT_DIR/shots/mangrove-tile.png"
pick 5340 "$OUT_DIR/shots/lapis-wool.png"
pick 5410 "$OUT_DIR/shots/tuff-dust.png"
pick 5480 "$OUT_DIR/shots/niobium-mesh.png"
pick 5550 "$OUT_DIR/shots/reed-foam.png"
pick 5620 "$OUT_DIR/shots/topaz-wool.png"
pick 5690 "$OUT_DIR/shots/clinker-dust.png"
pick 5760 "$OUT_DIR/shots/tungsten-foil.png"
pick 5830 "$OUT_DIR/shots/seagrass-tile.png"
pick 5900 "$OUT_DIR/shots/jade-wool.png"
pick 5970 "$OUT_DIR/shots/pumice-foam.png"
pick 6040 "$OUT_DIR/shots/rhenium-foil.png"
pick 6110 "$OUT_DIR/shots/willow-foam.png"
pick 6180 "$OUT_DIR/shots/garnet-wool.png"
pick 6250 "$OUT_DIR/shots/ash-tile.png"
pick 6320 "$OUT_DIR/shots/rust-hero-end.png"
echo "==> proof: $OUT_DIR/shots/full-frame.png"
ls -la "$OUT_DIR/shots"
