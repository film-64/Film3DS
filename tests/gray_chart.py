"""Exact digital-gray chart tests: no external display transforms are tested."""
import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("gray_chart", ROOT/"tools/gray_chart.py")
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class GrayFullFrameChartTests(unittest.TestCase):
    def test_codes_and_coverage(self):
        self.assertEqual(len(module.LEVELS),21)
        self.assertEqual(module.LEVELS[0],0)
        self.assertEqual(module.LEVELS[-1],255)
        self.assertEqual(len(set(module.LEVELS)),21)
        chart=module.generate("portrait-full")
        self.assertEqual(chart.size,(945,1260))
        self.assertEqual(chart.mode,"RGB")
        for i,code in enumerate(module.LEVELS):
            for x in (0,472,944):
                for y in (i*60,i*60+30,i*60+59):
                    self.assertEqual(chart.getpixel((x,y)),(code,code,code))
        landscape=module.generate("landscape-full")
        self.assertEqual(landscape.size,(1260,945))
        self.assertEqual(landscape.getpixel((0,0)),(0,0,0))
        self.assertEqual(landscape.getpixel((1259,0)),(255,255,255))
        # The rotated landscape chart has the same multiset of pixels.
        self.assertEqual(landscape.getpixel((630,472)),chart.getpixel((472,629)))

    def test_legacy_not_replaced(self):
        legacy=module.generate("classic")
        self.assertEqual(legacy.size,(1680,920))
        self.assertEqual(legacy.getpixel((0,150)),(0,0,0))
        self.assertEqual(legacy.getpixel((1679,150)),(255,255,255))
        self.assertEqual(legacy.getpixel((0,0)),(32,32,32))


if __name__=="__main__":
    unittest.main()
