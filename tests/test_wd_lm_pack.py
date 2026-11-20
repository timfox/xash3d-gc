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


if __name__ == "__main__":
	unittest.main()
