import sys
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import exposure_luma_transfer as f


class LumaTransferTests(unittest.TestCase):
    def synthetic(self, fps=5):
        times = [x + .75 for x in f.DENSE_COARSE[fps]]
        images = []
        for t in times:
            value = int(round(max(0, min(255, 45 + 55 * f.math.log2(t / times[2])))))
            images.append([f.np.full((16, 20), value, f.np.uint8) for _ in range(2)])
        anchors = [dict(index=i, images=[x.copy() for x in images[i]]) for i in (5, 8)]
        return images, times, anchors

    def test_profiles_pass_with_excluded_late_anchor_conditions(self):
        for fps in (5, 15):
            images, times, anchors = self.synthetic(fps)
            r = f.fit_profile(images, times, anchors)
            self.assertTrue(r['relative_luma_transfer_passed'], r)
            self.assertNotIn(8, r['training_indices'])
            self.assertEqual(r['holdout_indices'], list(range(1, len(times), 2)))
            self.assertTrue(all(x['passed'] for x in r['late_baseline_holdout']))

    def test_both_late_images_required_and_cannot_train_themselves(self):
        images, times, anchors = self.synthetic()
        anchors[0]['images'][0][:] += 25
        self.assertFalse(f.fit_profile(images, times, anchors)['relative_luma_transfer_passed'])

    def test_odd_holdout_corruption_rejects(self):
        images, times, anchors = self.synthetic()
        images[7][0][:] += 30
        self.assertFalse(f.fit_profile(images, times, anchors)['relative_luma_transfer_passed'])

    def test_excluded_coarse_condition_does_not_change_fit(self):
        images, times, anchors = self.synthetic()
        before = f.fit_profile(images, times, anchors)
        images[8] = [f.np.full_like(x, 0) for x in images[8]]
        self.assertEqual(before['model'], f.fit_profile(images, times, anchors)['model'])

    def test_out_of_domain_late_anchor_rejects_without_extrapolation(self):
        images, times, anchors = self.synthetic()
        anchors[1]['images'] = [f.np.full_like(x, 255) for x in anchors[1]['images']]
        r = f.fit_profile(images, times, anchors)
        self.assertFalse(r['relative_luma_transfer_passed'])
        self.assertIsNone(r['late_baseline_holdout'][1]['images'][0]['exposure_shift_EV'])

    def test_no_black_flat_or_insufficient_coverage_fallback(self):
        for value in (0, 100, 255):
            images, times, anchors = self.synthetic()
            images = [[f.np.full_like(x, value) for x in pair] for pair in images]
            self.assertFalse(f.fit_profile(images, times, anchors)['relative_luma_transfer_passed'])

    def test_unknown_source_grid_or_close_anchors_reject(self):
        images, times, anchors = self.synthetic()
        with self.assertRaises(ValueError):
            f.fit_profile(images, times, anchors, 'unknown')
        with self.assertRaises(ValueError):
            f.fit_profile(images[:10], times[:10], anchors)
        with self.assertRaises(ValueError):
            f.fit_profile(images, times, [anchors[0], anchors[0]])
        with self.assertRaises(ValueError):
            f.fit_profile(images, times, [anchors[0], dict(index=6, images=images[6])])

    def test_unverified_or_unknown_capture_rejects(self):
        for report in ({'passed': False}, {'passed': True, 'source': 'unknown'}):
            with patch.object(f, 'verify_dense', return_value=report):
                with self.assertRaises(ValueError):
                    f.analyse(Path('/nonexistent'))

    def test_gain_transfer_requires_two_valid_consistent_anchors(self):
        rows = [dict(core_code=528, ISP=128, coarse=x, effective_luma_shift_EV=v,
                     image_checks=[dict(exposure_shift_EV=v) for _ in range(2)]) for x, v in ((112, .01), (192, .02))]
        self.assertTrue(f.gain_transfer_checks(rows)[1]['passed'])
        rows[1]['effective_luma_shift_EV'] = .4
        rows[1]['image_checks'][0]['exposure_shift_EV'] = .4
        self.assertFalse(f.gain_transfer_checks(rows)[1]['passed'])
        rows[1]['effective_luma_shift_EV'] = None
        self.assertFalse(f.gain_transfer_checks(rows)[1]['passed'])

    def test_raw_luminance_reader_checks_size(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'roi'
            path.write_bytes(bytes([18, 128, 22, 128]) * 6144)
            y = f.image_y(path)
            self.assertEqual(y.shape, (96, 128))
            self.assertEqual(y[0, :2].tolist(), [18, 22])
            path.write_bytes(b'bad')
            with self.assertRaises(ValueError):
                f.image_y(path)


if __name__ == '__main__':
    unittest.main()
