"""Read-only neutral sampling through the real host loader and calibrated RAW."""
import hashlib
import json
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class NeutralTests(unittest.TestCase):
    def test_raw_patch_and_invalid_coordinates(self):
        source = ROOT / 'romfs/rawqa/gray.F3S'
        profile = ROOT / 'romfs/calibration/display-gray.f3isp'
        before = [hashlib.sha256(p.read_bytes()).digest() for p in (source, profile)]
        cmd = [str(ROOT / 'build/filmctl'), 'neutral-wb', str(source), str(profile)]
        result = subprocess.run(cmd + ['300', '240', '9', '9'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['accepted'], 81)
        self.assertEqual(report['rejected'], 0)
        self.assertLessEqual(report['residual_stops'], .08)
        self.assertLessEqual(report['spread_stops'], .15)
        self.assertIn(report['kelvin'], [0] + list(range(1000, 10001, 100)))
        self.assertTrue(-9 <= report['red'] <= 9 and -9 <= report['blue'] <= 9)
        for roi in (['-1','0','9','9'], ['0','0','0','9'], ['640','0','9','9'],
                    ['0','0','99999999999999999999999','9'], ['1x','0','9','9']):
            bad = subprocess.run(cmd + roi, capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertEqual(bad.stdout, '')
        self.assertEqual(before, [hashlib.sha256(p.read_bytes()).digest() for p in (source, profile)])

if __name__ == '__main__':
    unittest.main()
