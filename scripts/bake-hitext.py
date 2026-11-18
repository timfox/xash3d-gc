#!/usr/bin/env python3
"""Bake the GameCube hi-res text atlas: gfx/gc_hitext.bin

ASCII 32..126 rendered from a TrueType font into one 4-bit intensity (GX_TF_I4) texture, tiled the way GX
reads it (8x8 texels per 32-byte tile), plus a glyph table. The engine draws these as GX quads at 640x480,
so menu text is not limited by the 320x240 soft 2D buffer.

Font: Noto Sans (SIL Open Font License 1.1, https://openfontlicense.org), the build copies the licence text
next to the output (OFL.txt) because it is shipped with the font data. Any OFL / public-domain font works.

  scripts/bake-hitext.py [--font PATH] [--size PX] [--out gc_hitext.bin]

File layout (big endian):
  char[4] "HTXT"; u16 version=1; u16 tex_w; u16 tex_h; u16 line_h; u16 first; u16 count;
  count x { u16 x, y, w, h; s16 xoff, yoff; u16 adv }   (atlas rect, offset of the rect from the pen / line top, advance)
  tex data: I4, 8x8 tiles, left to right then top to bottom, two texels per byte (high nibble first)
"""
import argparse
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

FIRST, LAST = 32, 126


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--font", default="/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf")
	ap.add_argument("--size", type=int, default=36)
	ap.add_argument("--stroke", type=int, default=1, help="extra weight, px of stroke")
	ap.add_argument("--width", type=int, default=512)
	ap.add_argument("--height", type=int, default=256)
	ap.add_argument("--out", default="gc_hitext.bin")
	args = ap.parse_args()

	font = ImageFont.truetype(args.font, args.size)
	ascent, descent = font.getmetrics()
	line_h = ascent + descent + args.stroke * 2
	atlas = Image.new("L", (args.width, args.height), 0)
	draw = ImageDraw.Draw(atlas)
	glyphs = []
	x = y = 1
	row_h = 0
	for ch in range(FIRST, LAST + 1):
		c = chr(ch)
		adv = int(round(font.getlength(c))) + args.stroke
		bbox = font.getbbox(c, stroke_width=args.stroke)  # relative to the pen at the line top
		w = max(0, bbox[2] - bbox[0])
		h = max(0, bbox[3] - bbox[1])
		if c == " " or w == 0 or h == 0:
			glyphs.append((0, 0, 0, 0, 0, 0, adv))
			continue
		if x + w + 2 > args.width:
			x = 1
			y += row_h + 2
			row_h = 0
		if y + h + 1 > args.height:
			sys.exit("atlas too small: raise --height or lower --size")
		draw.text((x - bbox[0], y - bbox[1]), c, font=font, fill=255, stroke_width=args.stroke, stroke_fill=255)
		glyphs.append((x, y, w, h, bbox[0], bbox[1], adv))
		x += w + 2
		row_h = max(row_h, h)

	out = bytearray(b"HTXT")
	out += struct.pack(">HHHHHH", 1, args.width, args.height, line_h, FIRST, LAST - FIRST + 1)
	for g in glyphs:
		out += struct.pack(">HHHHhhH", *g)
	px = atlas.load()
	for ty in range(0, args.height, 8):
		for tx in range(0, args.width, 8):
			for yy in range(8):
				for xx in range(0, 8, 2):
					a = px[tx + xx, ty + yy] >> 4
					b = px[tx + xx + 1, ty + yy] >> 4
					out.append((a << 4) | b)
	with open(args.out, "wb") as f:
		f.write(out)
	print("wrote %s: %dx%d I4, line_h=%d, %d bytes, %d glyphs" % (args.out, args.width, args.height, line_h, len(out), len(glyphs)))


if __name__ == "__main__":
	main()
