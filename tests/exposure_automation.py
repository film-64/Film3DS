"""Exposure-state/photometry contracts; no simulated data labelled hardware."""
import copy
import math
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import decode_exposure_state as decoder
import exposure_register_contract as contract
import exposure_session as session


def evidence_fixture():
    stage = dict(sensor="1", phase="4", label="fixture", kind="code_baseline", settle="8", measured="12",
                 baseline_phase="0", write_mask="31", coarse="16", gain_gr="544", gain_b="586", gain_r="549", gain_gb="544", locked="1")
    values = dict.fromkeys(contract.REGISTERS, 0)
    values.update({"3012": 16, "3014": 633, "300a": 704, "300c": 843, "0010": 273,
                   "0012": 0, "0014": 45130, "3016": 1, "3028": 8,
                   "3056": 544, "3058": 586, "305a": 549, "305c": 544,
                   "302a": 8, "302c": 8, "302e": 4, "3030": 8,
                   "3032": 256, "3034": 256, "3036": 256, "3038": 256,
                   "32d4": 183, "32d6": 159, "32d8": 159, "32da": 133,
                   "a11d": 0, "a11f": 0})
    reads = [dict(sensor="1", phase="4", frame=str(frame), when=when, space=space, addr=address,
                  value=str(values[address]), result="00000000")
             for frame in range(8, 20) for when in ("before", "after") for space, address in contract.REQUIRED]
    frames = [dict(frame=str(frame), measured="1", ok="1", received="1", result="00000000") for frame in range(8, 20)]
    return stage, reads, frames


class AutomationTests(unittest.TestCase):
    def test_photographic_reference_preserves_meters_and_interval(self):
        reference = {"illumination_batch": "same", "timing": "after_capture", "readings": [
            {"meter": "A", "iso": 100, "f_number": 2.8, "shutter_seconds_interval": [.25, .5], "cct_kelvin": 3400},
            {"meter": "B", "iso": 100, "f_number": 2.8, "shutter_seconds": .5, "cct_kelvin": 4100}]}
        values = session.exposure_reference_values(reference, "same")
        self.assertEqual(values[0]["shutter_seconds_interval"], [.25, .5])
        self.assertAlmostEqual(values[0]["ev100_interval"][0], math.log2(15.68))
        self.assertAlmostEqual(values[0]["ev100_interval"][1], math.log2(31.36))
        self.assertEqual(values[1]["ev100_interval"], [math.log2(15.68)] * 2)
        self.assertTrue(values[0]["batch_bound"])
        self.assertFalse(values[0]["camera_iso_calibrated"])
        self.assertFalse(session.exposure_reference_values(reference, "different")[0]["batch_bound"])

    def test_invalid_exposure_reference_fails(self):
        for interval in ([.5, .25], [0, .5], [.25], [.25, float("nan")]):
            with self.assertRaises(ValueError):
                session.exposure_reference_values({"iso": 100, "f_number": 2.8,
                                                   "shutter_seconds_interval": interval}, None)

    def setUp(self):
        self.stage, self.reads, self.frames = evidence_fixture()

    def result(self):
        return decoder.verify_stage(self.stage, self.reads, self.frames)

    def test_complete_core_and_isp_are_decoded_separately(self):
        result = self.result()
        self.assertEqual(result["proof_status"], "verified_locked_sample")
        self.assertTrue(result["core_digital_unity_configured"])
        self.assertTrue(result["isp_digital_nonunity_configured"])
        self.assertEqual(result["isp_digital"]["R"]["configured_factor"], 183 / 128)
        self.assertEqual(result["isp_digital"]["B"]["configured_factor"], 133 / 128)
        self.assertFalse(result["no_end_to_end_digital_amplification_proven"])
        self.assertFalse(result["analog_multiplier_calibrated"])

    def test_missing_blue_or_greenb_cannot_pass(self):
        for address in ["32da", "3038"]:
            stage, reads, frames = evidence_fixture()
            reads = [r for r in reads if not (r["addr"] == address and r["frame"] == "8" and r["when"] == "before")]
            self.assertEqual(decoder.verify_stage(stage, reads, frames)["proof_status"], "rejected")

    def test_duplicate_readback_rejected(self):
        self.reads.append(copy.deepcopy(self.reads[0]))
        self.assertEqual(self.result()["proof_status"], "rejected")

    def test_isp_drift_rejected(self):
        next(r for r in self.reads if r["addr"] == "32d6" and r["frame"] == "19" and r["when"] == "after")["value"] = "160"
        self.assertEqual(self.result()["proof_status"], "rejected")

    def test_fake_lock_or_requested_control_rejected(self):
        for address in ["a11d", "a11f", "3012"]:
            stage, reads, frames = evidence_fixture()
            for r in reads:
                if r["addr"] == address:
                    r["value"] = "1"
            self.assertEqual(decoder.verify_stage(stage, reads, frames)["proof_status"], "rejected")

    def test_missing_frame_and_positive_timeout_rejected(self):
        self.frames.pop()
        self.assertEqual(self.result()["proof_status"], "rejected")
        self.stage, self.reads, self.frames = evidence_fixture()
        self.frames[0]["result"] = "09401bfe"
        self.assertEqual(self.result()["proof_status"], "rejected")

    def test_unknown_kind_mask_and_scope_rejected(self):
        for key, value in [("kind", "hidden_retry"), ("write_mask", "255"), ("measured", "100")]:
            stage, reads, frames = evidence_fixture()
            stage[key] = value
            with self.assertRaises(ValueError):
                decoder.verify_stage(stage, reads, frames)

    def test_ram_version_bounds_remain_explicit(self):
        body = "run ram:/fixture\ncomplete 1\nfailed 0\ncount 2\n0 a.csv 11000000 00000000\n1 b.csv 11000000 00000000\n"
        with self.assertRaises(ValueError):
            session.parse_index(("FILMRAM 1\n" + body).encode())
        self.assertEqual(len(session.parse_index(("FILMRAM 2\n" + body).encode())[1]), 2)
        self.assertEqual(len(session.parse_index(("FILMRAM 3\n" + body).encode())[1]), 2)
        with self.assertRaises(ValueError):
            session.parse_index(("FILMRAM 4\n" + body).encode())
        for version, limit in ((1, 40), (2, 40), (3, 320)):
            def index(count):
                return (f"FILMRAM {version}\nrun ram:/fixture\ncomplete 1\nfailed 0\ncount {count}\n" +
                        "".join(f"{i} f{i}.csv 1 00000000\n" for i in range(count))).encode()
            self.assertEqual(len(session.parse_index(index(limit))[1]), limit)
            with self.assertRaises(ValueError):
                session.parse_index(index(limit + 1))

    def test_yuyv_decode_matches_native_neutral_meter(self):
        with tempfile.TemporaryDirectory() as d:
            raw = Path(d) / "neutral.yuyv"
            raw.write_bytes(bytes([118, 128, 118, 128]) * (640 * 480 // 2))
            rgb = session.yuyv_rgb(raw, 1)
            measured = session.meter_rgb(rgb)["linear_mean"]
            self.assertAlmostEqual(measured, .18116424424986022, places=12)
            jpg = Path(d) / "neutral.jpg"
            Image.fromarray(np.rint(rgb * 255).astype(np.uint8)).save(jpg, quality=95, subsampling=0)
            with Image.open(jpg) as image:
                jpeg_mean = session.meter_rgb(np.asarray(image) / 255)["linear_mean"]
            self.assertLess(abs(math.log2(jpeg_mean / measured)), .05)
            with self.assertRaises(ValueError):
                session.yuyv_rgb(raw, 99)
            raw.write_bytes(b"truncated")
            with self.assertRaises(ValueError):
                session.yuyv_rgb(raw, 1)

    def test_no_end_to_end_unity_claim_even_when_both_configs_unity(self):
        for r in self.reads:
            if r["addr"] in contract.ISP_DIGITAL.values():
                r["value"] = "128"
        result = self.result()
        self.assertFalse(result["isp_digital_nonunity_configured"])
        self.assertFalse(result["no_end_to_end_digital_amplification_proven"])

    def test_short_frame_contract_requires_explicit_scope(self):
        self.stage.update(settle="2",measured="4")
        self.reads=[{**r,"frame":str(int(r["frame"])-6)} for r in self.reads if int(r["frame"])<12]
        self.frames=[{**r,"frame":str(int(r["frame"])-6)} for r in self.frames if int(r["frame"])<12]
        with self.assertRaises(ValueError):
            self.result()
        result=decoder.verify_stage(self.stage,self.reads,self.frames,"exposure-auto/v3")
        self.assertEqual(result["proof_status"],"verified_locked_sample")
        self.assertEqual(result["readbacks_per_register_expected"],8)


if __name__ == "__main__":
    unittest.main(verbosity=2)
