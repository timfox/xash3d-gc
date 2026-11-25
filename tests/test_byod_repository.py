"""Content-boundary checks using synthetic paths and temporary repositories."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/check-byod-repository.py'
spec = importlib.util.spec_from_file_location('byod_policy', SCRIPT)
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)

class ByodPolicyTests(unittest.TestCase):
    def test_source_and_license_files_remain_allowed(self):
        self.assertEqual(policy.violations(['engine/common/model.c', 'LICENSE',
            'scripts/gamecube-exports/gamecube_server_exports.inc',
            'game_launch/icon-xash-material.png']), [])

    def test_raw_data_converted_exports_and_captures_are_rejected(self):
        paths = ['custom/maps/test.BSP', 'custom/pak0.pak', 'exports/ui.png',
                 '.ai/shot.png', 'custom/movie.gcvid', 'custom/audio.gcpcm',
                 'docs/notes/menu-shots/logo.png', 'custom/world.gct']
        self.assertEqual(policy.violations(paths), sorted(paths))

    def test_force_added_asset_cannot_hide_behind_gitignore(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(['git', 'init', '-q', tmp], check=True)
            (root / '.gitignore').write_text('*.bsp\n')
            (root / 'synthetic.bsp').write_bytes(b'not game data')
            subprocess.run(['git', '-C', tmp, 'add', '-f', 'synthetic.bsp'], check=True)
            result = subprocess.run(['python3', str(SCRIPT)], cwd=tmp,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('synthetic.bsp', result.stdout)

if __name__ == '__main__':
    unittest.main()
