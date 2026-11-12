"""Real-evidence numerical validation and fail-closed analysis checks."""
import copy
import hashlib
import json
import math
from pathlib import Path
import unittest

import numpy as np
import exposure_model as model


class ExposureModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.result = json.loads((model.ROOT / "model.json").read_text())
        cls.coefficients = cls.result["response"]["coefficients"]

    def test_real_exposure_point_holdouts(self):
        rows = self.result["validation"]["leave_one_exposure_out"]
        self.assertEqual([r["actual_coarse"] for r in rows], [16, 64, 176])
        self.assertLess(max(abs(r["error_ev"]) for r in rows), 0.25)

    def test_independent_30fps_data_not_in_training(self):
        training_phases = {p[2] for p in self.result["response"]["training_points_coarse_y_phase"]}
        validation = self.result["validation"]["cross_30fps_unfitted"]
        self.assertEqual(len(validation), 3)
        self.assertFalse(training_phases.intersection(r["phase"] for r in validation))
        self.assertLess(max(abs(r["error_ev"]) for r in validation), 0.25)

    def test_held_out_frames_within_declared_tolerance(self):
        checks = self.result["validation"]
        self.assertGreaterEqual(checks["temporal_holdout_frames"], 18)
        self.assertLess(checks["temporal_holdout_max_abs_ev"], 0.25)

    def test_pixel_statistics_agree_with_original_evidence(self):
        rows = self.result["validation"]["raw_pgm_roi_checks"]
        expected = {f"offline-full-01/recovered/s1-p{p:03d}-cal_r0_t{c}.pgm"
                    for p, c in [(21, 4), (23, 16), (25, 64), (27, 176), (29, 528)]}
        expected.update(f"offline-full-01/recovered/s1-p{p:03d}-cal_r8_t{c}.pgm"
                        for p, c in [(11, 64), (13, 129), (15, 388)])
        actual = {r["file"] for r in rows}
        self.assertEqual({p for p in actual if p.startswith("offline-full-01/")}, expected)
        self.assertEqual(len([p for p in actual if p.startswith("ram-codes-fine-01/")]), 13)
        self.assertLess(max(abs(r["last_frame_mean_error"]) for r in rows), 1e-7)

    def test_exposure_decision_against_independent_captures(self):
        rows = self.result["validation"]["exposure_decision_30fps_pairs"]
        self.assertEqual(len(rows), 6)
        self.assertLess(max(abs(r["error_ev"]) for r in rows), 0.25)
        for c in [0, -1, math.nan, math.inf]:
            with self.subTest(coarse=c), self.assertRaises(ValueError):
                model.solve_fixed_gain(self.coefficients, c, 75, 118)

    def test_no_dark_saturation_nan_or_infinity_extrapolation(self):
        for y in [-1, 0, 9.13, 18.16, 235, 255, math.nan, math.inf]:
            with self.subTest(y=y), self.assertRaises(ValueError):
                model.value(self.coefficients, y)

    def test_inverse_is_monotone_in_entire_allowed_domain(self):
        values = [model.value(self.coefficients, float(y)) for y in
                  np.linspace(*self.result["response"]["valid_y"], 1000)]
        self.assertTrue(all(b > a > 0 for a, b in zip(values, values[1:])))

    def test_drift_and_incomplete_proof_are_rejected(self):
        stage = {"complete": True, "locked_controls_held": True, "measured_rows": 12,
                 "registers": {"3012": {"complete": True, "constant": True, "values": [7] * 12}}}
        for key, invalid in [("complete", False), ("locked_controls_held", False), ("measured_rows", 11)]:
            bad = copy.deepcopy(stage)
            bad[key] = invalid
            with self.subTest(key=key), self.assertRaises(ValueError):
                model.checked(bad)
        bad = copy.deepcopy(stage)
        bad["registers"]["3012"]["values"][-1] = 8
        with self.assertRaises(ValueError):
            model.constant(bad, "3012")

    def test_reference_parameters_do_not_masquerade_as_absolute_iso(self):
        reference = self.result["reference_ei_model"]
        self.assertFalse(reference["absolute_iso_verified"])
        self.assertFalse(reference["cross_illumination_transfer_verified"])
        self.assertFalse(self.result["timing"]["absolute_optical_time_verified"])
        rows = reference["gain_rows"]
        self.assertTrue(all(r["nominal_reference_ei"] is None for r in rows[:2]))
        self.assertTrue(all(r["nominal_reference_ei"] > 0 for r in rows[2:]))
        self.assertTrue(all(r["fine_plus_sensitivity_ei"] < r["nominal_reference_ei"] for r in rows[2:]))

    def test_saved_inputs_unchanged(self):
        for name, expected in self.result["input_sha256"].items():
            with self.subTest(name=name):
                actual = hashlib.sha256((model.BASE / name).read_bytes()).hexdigest()
                self.assertEqual(actual, expected)

    def test_5fps_unsupported_points_not_counted_as_pass(self):
        checks = self.result["validation"]["slow_baseline_scaled_unfitted"]
        rejected = [r for r in checks if r["status"] == "outside_response_domain"]
        self.assertGreaterEqual(len(rejected), 3)
        self.assertTrue(all(r["error_ev"] is None for r in rejected))

    def test_plan_selects_real_complete_combination(self):
        plan = model.plan_observed_setting(self.result, 30, 64, 57.326049805, 88.677205405)
        self.assertEqual(plan["selected_observed_combination"]["coarse"], 129)
        self.assertIn(plan["selected_observed_combination"], self.result["observed_complete_controls"])
        self.assertFalse(plan["hardware_applied"])
        for args in [(5, 64, 79, 118), (30, 65534, 75, 118), (30, 64, 9, 118), (30, 388, 21, 200)]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                model.plan_observed_setting(self.result, *args)

    def test_every_exported_control_has_complete_measured_stage(self):
        rows = self.result["observed_complete_controls"]
        self.assertGreaterEqual(len(rows), 25)
        self.assertTrue(all(r["measured_frames"] == 12 and len(r["four_channel_codes"]) == 4 for r in rows))
        self.assertFalse(any(r["coarse"] >= 65534 for r in rows))


if __name__ == "__main__":
    unittest.main(verbosity=2)
