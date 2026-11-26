#!/usr/bin/env python3
"""Generate gc_crabhall.map: a small Black Mesa hallway + headcrab arena.

Retail Half-Life textures only (halflife.wad, referenced, never embedded) and
retail entities only. Compile with any ZHLT/VHLT/SDHLT toolchain, e.g.:

    hlcsg -wadautodetect gc_crabhall && hlbsp gc_crabhall \
        && hlvis -full gc_crabhall && hlrad -extra gc_crabhall

(hlcsg needs halflife.wad on its search path; the BSP only references it.)

Run:  python3 maps/src/gen_gc_crabhall.py > maps/src/gc_crabhall.map
"""
import sys

UP = (0, 0, 1)


def cross(a, b):
	return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def fmt(v):
	return " ".join(str(int(c)) if float(c).is_integer() else f"{c:g}" for c in v)


def face_axes(n):
	"""Valve 220 axes that read left-to-right for a viewer facing the face."""
	if n[2] != 0:
		return (1, 0, 0), (0, -1, 0)
	right = cross((-n[0], -n[1], -n[2]), UP)
	return right, (0, 0, -1)


def box(x1, y1, z1, x2, y2, z2, tex, side_tex=None, top_tex=None, bottom_tex=None):
	"""Axis-aligned brush. tex may be a dict keyed by +x -x +y -y +z -z."""
	if isinstance(tex, str):
		t = {k: tex for k in ("+x", "-x", "+y", "-y", "+z", "-z")}
		if side_tex:
			for k in ("+x", "-x", "+y", "-y"):
				t[k] = side_tex
		if top_tex:
			t["+z"] = top_tex
		if bottom_tex:
			t["-z"] = bottom_tex
	else:
		t = tex
	lo, hi = (x1, y1, z1), (x2, y2, z2)
	corners = [(x, y, z) for x in (x1, x2) for y in (y1, y2) for z in (z1, z2)]
	faces = [
		# plane points, normal, key (TrenchBroom winding)
		((lo, (lo[0], lo[1] + 1, lo[2]), (lo[0], lo[1], lo[2] + 1)), (-1, 0, 0), "-x"),
		((lo, (lo[0], lo[1], lo[2] + 1), (lo[0] + 1, lo[1], lo[2])), (0, -1, 0), "-y"),
		((lo, (lo[0] + 1, lo[1], lo[2]), (lo[0], lo[1] + 1, lo[2])), (0, 0, -1), "-z"),
		((hi, (hi[0], hi[1] + 1, hi[2]), (hi[0] + 1, hi[1], hi[2])), (0, 0, 1), "+z"),
		((hi, (hi[0] + 1, hi[1], hi[2]), (hi[0], hi[1], hi[2] + 1)), (0, 1, 0), "+y"),
		((hi, (hi[0], hi[1], hi[2] + 1), (hi[0], hi[1] + 1, hi[2])), (1, 0, 0), "+x"),
	]
	out = ["{"]
	for pts, n, key in faces:
		u, v = face_axes(n)
		on_face = [c for c in corners if dot(c, n) == max(dot(k, n) for k in corners)]
		# Align the texture's top-left corner with the face's top-left corner.
		uoff = -min(dot(c, u) for c in on_face)
		voff = -min(dot(c, v) for c in on_face)
		out.append(
			f"( {fmt(pts[0])} ) ( {fmt(pts[1])} ) ( {fmt(pts[2])} ) {t[key]} "
			f"[ {fmt(u)} {fmt((uoff,))} ] [ {fmt(v)} {fmt((voff,))} ] 0 1 1"
		)
	out.append("}")
	return "\n".join(out)


def ent(classname, origin=None, brushes=(), **kv):
	lines = ["{", f'"classname" "{classname}"']
	if origin is not None:
		lines.append(f'"origin" "{fmt(origin)}"')
	for k, v in kv.items():
		lines.append(f'"{k}" "{v}"')
	lines.extend(brushes)
	lines.append("}")
	return "\n".join(lines)


# ---------------------------------------------------------------- textures
CORR_WALL = "C1A1_W1"          # Unforeseen Consequences cream wall panels
CORR_FLOOR = "C1A1_FLR2B"      # c1a1 grey floor tiles
CEIL = "FIFTIES_CEIL01"        # acoustic ceiling tile
CEIL_LIGHT = "+0~FIFTS_LGHT01" # fluorescent ceiling panel
TRIM = "DRKMTLT_BORD11"
LAB_A, LAB_B, LAB_C = "C1A0_LABW4", "C1A0_LABW5", "C1A0_LABW1"  # Anomalous Materials lab walls
LAB_FLOOR = "C1A0_LABFLR"
DOOR = "C1A1_DR1"
DOOR_EDGE = "C1A1DOOREDGE"
CRATE_S, CRATE_L = "CRATE02", "CRATE13"

W = 16  # wall thickness

world = []

# ---------------------------------------------------------------- corridor A (spawn hall, runs +X)
# interior x 0..768, y -64..64, z 0..128
world.append(box(-W, -64 - W, -W, 768 + W, 64 + W, 0, CORR_FLOOR))
world.append(box(-W, -64 - W, 128, 768 + W, 64 + W, 128 + W, CEIL))
world.append(box(-W, -64, 0, 0, 64, 128, CORR_WALL))                    # west end
world.append(box(-W, -64 - W, 0, 768 + W, -64, 128, CORR_WALL))         # south
world.append(box(-W, 64, 0, 640, 64 + W, 128, CORR_WALL))               # north (open at the turn)
world.append(box(768, -64, 0, 768 + W, 512, 128, CORR_WALL))            # east (A and B)

# ---------------------------------------------------------------- corridor B (runs +Y to the arena)
# interior x 640..768, y 64..512, z 0..128
world.append(box(640 - W, 64, -W, 768 + W, 512 + W, 0, CORR_FLOOR))
world.append(box(640 - W, 64, 128, 768 + W, 512 + W, 128 + W, CEIL))
world.append(box(640 - W, 64 + W, 0, 640, 512, 128, CORR_WALL))         # west

# ceiling light panels (4 units proud of the ceiling)
corr_lights = [(x, 0) for x in (128, 320, 512, 704)] + [(704, y) for y in (224, 416)]
for (cx, cy) in corr_lights:
	rot = cx == 704 and cy > 64
	hx, hy = (40, 32) if rot else (32, 40)
	world.append(box(cx - hx, cy - hy, 124, cx + hx, cy + hy, 128, TRIM, bottom_tex=CEIL_LIGHT))

# ---------------------------------------------------------------- arena (one room)
# interior x 320..1088, y 528..1296, z 0..192 ; door wall y 512..528
AX1, AX2, AY1, AY2, AH = 320, 1088, 528, 1296, 192
world.append(box(AX1 - W, AY1 - W, -W, AX2 + W, AY2 + W, 0, LAB_FLOOR))
world.append(box(AX1 - W, AY1 - W, AH, AX2 + W, AY2 + W, AH + W, CEIL))

LAB_CYCLE = (LAB_A, LAB_B, LAB_A, LAB_C, LAB_C, LAB_B)


def lab_wall(axis, fixed1, fixed2, start, end, skip=None):
	"""Wall split into 128-unit lab panels (z 0..160) with a dark trim band above."""
	i = 0
	pos = start
	while pos < end:
		nxt = min(pos + 128, end)
		tex = LAB_CYCLE[i % len(LAB_CYCLE)]
		if not (skip and skip[0] < nxt and pos < skip[1]):
			if axis == "x":
				world.append(box(pos, fixed1, 0, nxt, fixed2, 160, tex))
			else:
				world.append(box(fixed1, pos, 0, fixed2, nxt, 160, tex))
		pos = nxt
		i += 1
	if axis == "x":
		world.append(box(start, fixed1, 160, end, fixed2, AH, TRIM))
	else:
		world.append(box(fixed1, start, 160, fixed2, end, AH, TRIM))


# door wall (south): doorway x 656..752, z 0..96
DX1, DX2, DZ = 656, 752, 96
world.append(box(AX1 - W, AY1 - W, 0, DX1, AY1, 160, LAB_A))
world.append(box(DX2, AY1 - W, 0, AX2 + W, AY1, 160, LAB_B))
world.append(box(DX1, AY1 - W, DZ, DX2, AY1, 160, LAB_A))
world.append(box(AX1 - W, AY1 - W, 160, AX2 + W, AY1, AH, TRIM))
lab_wall("x", AY2, AY2 + W, AX1 - W, AX2 + W)          # north
lab_wall("y", AX1 - W, AX1, AY1, AY2)                  # west
lab_wall("y", AX2, AX2 + W, AY1, AY2)                  # east

# central column
world.append(box(640, 848, 0, 768, 976, 160, LAB_B))
world.append(box(640, 848, 160, 768, 976, AH, TRIM))

# crates for cover
world.append(box(384, 1120, 0, 480, 1216, 96, CRATE_L))
world.append(box(400, 1136, 96, 464, 1200, 160, CRATE_S))
world.append(box(928, 608, 0, 1024, 704, 96, CRATE_L))
world.append(box(896, 1120, 0, 960, 1184, 64, CRATE_S))
world.append(box(960, 1120, 0, 1024, 1184, 64, CRATE_S))
world.append(box(928, 1120, 64, 992, 1184, 128, CRATE_S))
world.append(box(448, 720, 0, 512, 784, 64, CRATE_S))

arena_lights = [(x, y) for x in (448, 704, 960) for y in (656, 912, 1168) if (x, y) != (704, 912)]
for (cx, cy) in arena_lights:
	world.append(box(cx - 32, cy - 40, AH - 4, cx + 32, cy + 40, AH, TRIM, bottom_tex=CEIL_LIGHT))

# ---------------------------------------------------------------- entities
ents = []
ents.append(ent("info_player_start", (64, 0, 36), angle="0"))

# light fixtures
for (cx, cy) in corr_lights:
	ents.append(ent("light", (cx, cy, 104), _light="255 240 210 120"))
for (cx, cy) in arena_lights:
	ents.append(ent("light", (cx, cy, 164), _light="255 240 215 200"))
ents.append(ent("light", (704, 496, 100), _light="255 220 170 60"))     # over the arena door

# kit: retail pickups only
ents.append(ent("item_suit", (176, 0, 16), spawnflags="1", angle="0"))
ents.append(ent("weapon_crowbar", (256, 32, 16), angle="90"))
ents.append(ent("weapon_9mmhandgun", (320, -32, 16), angle="270"))
ents.append(ent("ammo_9mmclip", (368, -40, 16)))
ents.append(ent("ammo_9mmclip", (368, -16, 16)))
ents.append(ent("item_battery", (448, 40, 16)))
ents.append(ent("weapon_shotgun", (704, 16, 16), angle="90"))
ents.append(ent("ammo_buckshot", (736, -32, 16)))
ents.append(ent("item_healthkit", (720, 288, 16)))
ents.append(ent("ammo_buckshot", (672, 320, 16)))

ents.append(ent("weapon_9mmAR", (576, 600, 16), angle="90"))
ents.append(ent("ammo_9mmAR", (544, 640, 16)))
ents.append(ent("ammo_9mmAR", (848, 600, 16)))
ents.append(ent("ammo_buckshot", (880, 600, 16)))
ents.append(ent("item_healthkit", (1048, 576, 16)))
ents.append(ent("item_healthkit", (360, 1256, 16)))
ents.append(ent("item_healthkit", (1048, 1256, 16)))
ents.append(ent("item_battery", (1048, 1000, 16)))

# the headcrabs
crabs = [
	(704, 440, 270),                                   # one waiting in the hall
	(480, 880, 0), (560, 1040, 0), (880, 800, 180), (880, 1040, 180),
	(1000, 900, 180), (400, 1000, 0), (560, 640, 90), (820, 680, 90),
	(1000, 1250, 270), (640, 1250, 270), (760, 1100, 270), (560, 800, 0),
	(420, 1260, 270), (1040, 760, 180),
]
for (x, y, a) in crabs:
	ents.append(ent("monster_headcrab", (x, y, 8), angle=str(a)))

# arena door: retail sliding door, rises into the wall
ents.append(ent(
	"func_door", None,
	[box(DX1, 516, 0, DX2, 524, DZ, {"+y": DOOR, "-y": DOOR, "+x": DOOR_EDGE, "-x": DOOR_EDGE,
		"+z": DOOR_EDGE, "-z": DOOR_EDGE})],
	angle="-1", speed="100", wait="4", lip="8", movesnd="1", stopsnd="1",
))

# lab hum, as heard all over Black Mesa
ents.append(ent("ambient_generic", (704, 912, 176), message="ambience/labdrone2.wav",
	health="3", spawnflags="8"))

print("{")
print('"classname" "worldspawn"')
print('"mapversion" "220"')
print('"message" "Headcrab Hall (GameCube test map)"')
print('"wad" "\\valve\\halflife.wad"')
for b in world:
	print(b)
print("}")
for e in ents:
	print(e)
