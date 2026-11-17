import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import exposure_luma_validate as f


class FixedLumaValidationTests(unittest.TestCase):
    model = dict(mean_Y=[40, 80, 120, 160, 200], log2_effective_lines=[0, 1, 2, 3, 4])

    def pair(self, value):
        return [f.np.full((12, 16), value, f.np.uint8) for _ in range(2)]

    def test_only_reference_start_defines_offset(self):
        offset = f.reference_offset(self.pair(100), 2, self.model)
        self.assertEqual(offset, .5)
        self.assertTrue(f.check_observation(self.pair(140), 4, self.model, offset)['passed'])
        self.assertFalse(f.check_observation(self.pair(160), 4, self.model, offset)['passed'])

    def test_both_images_must_pass_without_mean_cancellation(self):
        images = self.pair(120)
        images[0][:] = 100
        images[1][:] = 140
        self.assertFalse(f.check_observation(images, 4, self.model, 0)['passed'])

    def test_gain_uses_fixed_expected_shift(self):
        self.assertTrue(f.check_observation(self.pair(160), 4, self.model, 0, 1)['passed'])
        self.assertFalse(f.check_observation(self.pair(160), 4, self.model, 0, .5)['passed'])

    def test_no_reference_or_holdout_extrapolation(self):
        with self.assertRaises(ValueError):
            f.reference_offset(self.pair(30), 2, self.model)
        checked = f.check_observation(self.pair(210), 4, self.model, 0)
        self.assertFalse(checked['passed'])
        self.assertIsNone(checked['residuals_EV'])

    def test_reference_repeat_instability_rejects(self):
        images = self.pair(100)
        images[1][:] = 120
        with self.assertRaises(ValueError):
            f.reference_offset(images, 2, self.model)

    def fixed_file(self, directory):
        path = Path(directory) / 'model.json'
        data = dict(schema=f.EXPOSURE_LUMA_SOURCE, manifest_sha256=hashlib.sha256(b'manifest').hexdigest(),
                    profiles=[dict(fps=fps, relative_luma_transfer_passed=True) for fps in (5, 15)])
        path.write_text(json.dumps(data))
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def test_model_hash_and_source_and_pass_proof_required(self):
        with tempfile.TemporaryDirectory() as d:
            path, sha = self.fixed_file(d)
            self.assertEqual(f.load_model(path, sha)['schema'], f.EXPOSURE_LUMA_SOURCE)
            with self.assertRaises(ValueError):
                f.load_model(path, '0' * 64)
            data = json.loads(path.read_text())
            data['profiles'][0]['relative_luma_transfer_passed'] = False
            path.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                f.load_model(path, hashlib.sha256(path.read_bytes()).hexdigest())
            data['profiles'][0]['relative_luma_transfer_passed'] = 'true'
            path.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                f.load_model(path, hashlib.sha256(path.read_bytes()).hexdigest())

    def test_model_training_batch_cannot_validate_itself(self):
        with tempfile.TemporaryDirectory() as d:
            path, sha = self.fixed_file(d)
            (Path(d) / 'manifest.txt').write_bytes(b'manifest')
            report = dict(passed=True, source=f.CONTRACT_MATRIX[f.EXPOSURE_LUMA_VALIDATE_SOURCE]['input_source'])
            with patch.object(f, 'verify_dense', return_value=report):
                with self.assertRaisesRegex(ValueError, 'independent capture'):
                    f.analyse(d, path, sha)

    def test_unknown_validation_source_rejects_before_read(self):
        with self.assertRaises(ValueError):
            f.analyse('/nonexistent', '/nonexistent', '', 'unknown')

    def test_pipeline_match_ignores_live_gain_but_requires_config(self):
        rule = f.CONTRACT_MATRIX[f.EXPOSURE_LUMA_VALIDATE_SOURCE]['pipeline_match']
        snap = {k: 1 for k in rule['flat']}
        snap.update(matrix_mode={'2703': 640}, rebuild_timing={k: 2 for k in rule['rebuild_timing']})
        original = f.fingerprint(snap)
        snap['rebuild_timing']['32d4'] = 133
        self.assertEqual(original, f.fingerprint(snap))
        snap['3210'] = 2
        self.assertNotEqual(original, f.fingerprint(snap))
        del snap['3210']
        with self.assertRaises(KeyError):
            f.fingerprint(snap)


if __name__ == '__main__':
    unittest.main()
