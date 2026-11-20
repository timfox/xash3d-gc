"""World DL lightmap page packing (scripts/bake_worlddl_goldsrc.py LmPage)."""

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("bake_worlddl_goldsrc", ROOT / "scripts" / "bake_worlddl_goldsrc.py")
bake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bake)


def overlaps(a, b) -> bool:
	return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


class LmPackTest(unittest.TestCase):
	def check(self, dims):
		page = bake.LmPage()
		rects = page.pack(dims)
		self.assertEqual(len(rects), len(dims))
		placed = [r for r in rects if r]
		for r in placed:
			self.assertTrue(r[0] >= 0 and r[1] >= 0 and r[0] + r[2] <= 32 and r[1] + r[3] <= 32)
			self.assertFalse(overlaps(r, (0, 0, 2, 2)), "cell over the grey corner")
		for i, a in enumerate(placed):
			for b in placed[i + 1:]:
				self.assertFalse(overlaps(a, b))
		return rects

	def test_few_faces_keep_retail_size(self):
		rects = self.check([(9, 7), (5, 5), (3, 12)])
		self.assertEqual([(r[2], r[3]) for r in rects], [(9, 7), (5, 5), (3, 12)])

	def test_crowded_leaf_gives_every_face_a_cell(self):
		# ten full beach faces: the old packer lit three and left seven flat grey
		rects = self.check([(16, 16)] * 10)
		self.assertTrue(all(rects))
		self.assertEqual(len({(r[2], r[3]) for r in rects}), 1)
		self.assertGreaterEqual(rects[0][2], 8)

	def test_many_small_faces(self):
		rects = self.check([(3, 3)] * 400)
		self.assertTrue(all(rects))

	def test_overfull_page_reports_none(self):
		rects = self.check([(2, 2)] * 1100)
		self.assertTrue(all(r is None for r in rects))

	def check_pages(self, dims, npages):
		cells = bake.lm_pack_pages(dims, npages)
		self.assertEqual(len(cells), len(dims))
		by_page = {}
		for c in cells:
			if c:
				self.assertTrue(0 <= c[0] < npages)
				by_page.setdefault(c[0], []).append(c[1])
		for rects in by_page.values():
			for i, a in enumerate(rects):
				self.assertTrue(a[0] >= 0 and a[1] >= 0 and a[0] + a[2] <= 32 and a[1] + a[3] <= 32)
				self.assertFalse(overlaps(a, (0, 0, 2, 2)))
				for b in rects[i + 1:]:
					self.assertFalse(overlaps(a, b))
		return cells

	def test_crowded_leaf_keeps_retail_size_on_more_pages(self):
		dims = [(16, 16)] * 10
		self.assertEqual(bake.lm_pages_needed(dims), 4)
		cells = self.check_pages(dims, 4)
		self.assertTrue(all(cells))
		self.assertTrue(all(c[1][2] == 16 and c[1][3] == 16 for c in cells))
		self.assertEqual(len({c[0] for c in cells}), 4)

	def test_pages_needed(self):
		self.assertEqual(bake.lm_pages_needed([]), 0)
		self.assertEqual(bake.lm_pages_needed([(9, 7), (5, 5)]), 1)
		self.assertEqual(bake.lm_pages_needed([(16, 16)] * 5), 2)
		self.assertEqual(bake.lm_pages_needed([(17, 17)] * 40), bake.LM_CHUNK_PAGES)

	def test_one_page_still_scales_everything(self):
		cells = self.check_pages([(16, 16)] * 10, 1)
		self.assertTrue(all(cells))
		self.assertEqual({c[0] for c in cells}, {0})
		self.assertEqual(bake.lm_pack_pages([(4, 4)], 0), [None])

	def test_blit_same_size_is_exact_and_corners_hold_when_smaller(self):
		samples = [[(x * 10, y * 10, 7) for x in range(9)] for y in range(5)]
		page = bake.LmPage()
		page.blit((4, 4, 9, 5), samples)
		for y in range(5):
			for x in range(9):
				self.assertEqual(page.pix[4 + y][4 + x], samples[y][x])
		page.blit((20, 20, 2, 2), samples)
		# two texels cover nine luxels: each is a filtered half, darker left, brighter right
		self.assertLess(page.pix[20][20][0], page.pix[20][21][0])
		self.assertLess(page.pix[20][20][1], page.pix[21][20][1])
		page.blit((30, 30, 1, 1), samples)
		self.assertEqual(page.pix[30][30], (40, 20, 7))


class LmCmprTest(unittest.TestCase):
	def test_page_is_512_bytes_and_close_to_the_source(self):
		# a smooth lightmap: a diagonal gradient with a warm tint
		pix = [[(min(255, 40 + 5 * x + 2 * y), min(255, 36 + 5 * x + 2 * y), min(255, 30 + 4 * x + 2 * y)) for x in range(32)] for y in range(32)]
		data = bake.cmpr_page(pix)
		self.assertEqual(len(data), bake.LM_CMPR_BYTES)
		got = bake.cmpr_page_decode(data)
		worst = max(abs(got[y][x][k] - pix[y][x][k]) for y in range(32) for x in range(32) for k in range(3))
		self.assertLessEqual(worst, 12)

	def test_aligned_cells_own_their_blocks(self):
		dims = [(9, 7), (5, 5), (3, 12), (16, 16), (2, 2)]
		cells = bake.lm_pack_pages(dims, 2, 4)
		self.assertTrue(all(cells))
		owned = {}
		for n, (pg, (x, y, w, h)) in enumerate(cells):
			self.assertEqual((x % 4, y % 4), (0, 0))
			for by in range(y // 4, -(-(y + h) // 4)):
				for bx in range(x // 4, -(-(x + w) // 4)):
					self.assertNotEqual((bx, by), (0, 0), "cell in the grey block")
					self.assertNotIn((pg, bx, by), owned)
					owned[(pg, bx, by)] = n
		# a bright cell beside a dark one: neither bleeds into the other through a shared block
		page = bake.LmPage()
		page.blit((4, 0, 3, 3), [[(250, 250, 250)] * 3] * 3, 4)
		page.blit((8, 0, 3, 3), [[(10, 10, 10)] * 3] * 3, 4)
		got = bake.cmpr_page_decode(bake.cmpr_page(page.pix))
		self.assertGreaterEqual(got[1][5][0], 244)
		self.assertLessEqual(got[1][9][0], 16)
		self.assertTrue(120 <= got[0][0][0] <= 136)

	def test_flat_block_uses_colour_zero(self):
		block = bake.cmpr_block([(128, 128, 128)] * 16)
		self.assertEqual(block[4:], bytes(4))
		self.assertEqual(block[0:2], block[2:4])

	def test_end_colours_ordered_for_four_colour_mode(self):
		px = [(10 * i, 10 * i, 10 * i) for i in range(16)]
		c0, c1 = int.from_bytes(bake.cmpr_block(px)[0:2], "big"), int.from_bytes(bake.cmpr_block(px)[2:4], "big")
		self.assertGreater(c0, c1)


class LmGrantTest(unittest.TestCase):
	def test_every_chunk_gets_a_page_before_any_gets_two(self):
		needs = [("big", 4), ("mid", 2), ("small", 1), ("crowded", 3)]
		self.assertEqual(bake.lm_grant_pages(needs, 4), {"big": 1, "mid": 1, "small": 1, "crowded": 1})
		self.assertEqual(bake.lm_grant_pages(needs, 6), {"big": 2, "mid": 2, "small": 1, "crowded": 1})
		self.assertEqual(bake.lm_grant_pages(needs, 7), {"big": 2, "mid": 2, "small": 1, "crowded": 2})

	def test_budget_is_never_exceeded_and_needs_are_met_when_it_lasts(self):
		needs = [(i, 1 + i % 4) for i in range(40)]
		for budget in (0, 1, 39, 40, 57, 100, 1000):
			got = bake.lm_grant_pages(needs, budget)
			self.assertLessEqual(sum(got.values()), budget)
			self.assertTrue(all(got[c] <= n for c, n in needs if c in got))
		self.assertEqual(bake.lm_grant_pages(needs, 1000), dict(needs))

	def test_a_crowded_leaf_with_two_cmpr_pages_has_no_face_without_a_cell(self):
		dims = [(2, 2)] * 100	# 63 one-luxel cells is all a CMPR page holds
		self.assertFalse(all(bake.lm_pack_pages(dims, 1, 4)))
		self.assertTrue(all(bake.lm_pack_pages(dims, 2, 4)))
		self.assertGreaterEqual(bake.lm_pages_needed(dims, 4), 2)


if __name__ == "__main__":
	unittest.main()
