"""Synthetic response tests and real C evidence rejection, never fake hardware success."""
import copy
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT / "tools"))
import exposure_sweep as s
import exposure_sweep_contract as c


class ModelTests(unittest.TestCase):
    def test_registration_matches_compiled_header(self):
        self.assertEqual((ROOT / "build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h").read_text(), c.header())
        self.assertEqual(len(list(c.stages())), c.MAX_STAGES)
        self.assertEqual(len(set((code,isp) for _,code,isp in c.STATES)),25)

    def test_photographic_reference_and_binding(self):
        self.assertAlmostEqual(s.ev_reading(dict(iso=100,f_number=2.8,shutter_seconds=.25))[0],4.970853654,places=8)
        ref=dict(capture_id="a",batch_id="one",target_id="target",meter="A",before=dict(ev100=5),after=dict(ev100=5.05))
        self.assertEqual(s.bind_reference(dict(capture_id="a"),ref)[0],5)
        with self.assertRaises(s.EvidenceError):s.bind_reference(dict(capture_id="b"),ref)
        ref["after"]["ev100"]=5.5
        with self.assertRaises(s.EvidenceError):s.bind_reference(dict(capture_id="a"),ref)
        for v in (0,-1,math.inf,math.nan):
            with self.assertRaises(s.EvidenceError):s.ev_reading(dict(iso=100,f_number=2.8,shutter_seconds=v))

    @staticmethod
    def synthetic():
        times=np.array(c.COARSE,dtype=float)/1000
        # Known nonlinear response and a nonuniform static target. No unit-slope assumption.
        light=np.linspace(.015,.6,64*48).reshape(48,64,1)*np.array([.85,1.,.7])
        images=[np.rint(np.clip(light*t,0,1)**(1/2.2)*255).astype(np.uint8) for t in times]
        return times,images

    def test_nonlinear_response_and_unfitted_exposures(self):
        times,images=self.synthetic()
        r=s.fit_response(images,times)
        self.assertTrue(r["passed"],r["holdout"])
        self.assertTrue(set(r["training"]).isdisjoint(c.HOLDOUT_INDICES))
        response=np.array(r["response"])
        self.assertGreater(math.log2(response[160,1]/response[80,1]),1.8)

    def test_heldout_corruption_cannot_train_itself_away(self):
        times,images=self.synthetic()
        images[5]=np.rint(np.clip(images[5].astype(float)*1.3,0,255)).astype(np.uint8)
        r=s.fit_response(images,times)
        self.assertFalse(r["passed"])

    def test_single_point_and_saturation_do_not_create_model(self):
        with self.assertRaises(s.EvidenceError):s.fit_response([np.zeros((48,64,3),np.uint8)],[1])
        with self.assertRaises(s.EvidenceError):s.fit_response([np.full((48,64,3),255,np.uint8)]*10,c.COARSE)

    def test_invalid_times_and_partition_fail_before_fitting(self):
        times,images=self.synthetic()
        for value in (0,-1,math.inf,math.nan):
            invalid=times.copy();invalid[1]=value
            with self.assertRaises(s.EvidenceError):s.fit_response(images,invalid)
        with self.assertRaises(s.EvidenceError):s.fit_response(images,times,holdout_indices=(1,))
        with self.assertRaises(s.EvidenceError):s.validate_model(dict(schema="unregistered"))

    def test_meter_fraction_is_preserved(self):
        value=s.parse_meter("100,2.8,1/4")
        self.assertEqual(value["shutter_seconds"],.25)
        self.assertEqual(value["entered"],"100,2.8,1/4")
        self.assertEqual(s.parse_meter("EV100=5")["ev100"],5)
        interval=s.parse_meter("EV100=8.1:8.3")
        self.assertEqual(s.ev_bounds(interval),(8.1,8.3))
        reference=dict(capture_id="a",batch_id="b",target_id="target",meter="A",before=interval,after=interval)
        lo,width=s.bind_reference(dict(capture_id="a"),reference)
        self.assertEqual(lo,8.1);self.assertAlmostEqual(width,.2)
        for values in ([8.3,8.1],[math.nan,8.3],[8.1]):
            with self.assertRaises(s.EvidenceError):s.ev_bounds(dict(ev100_interval=values))

    def test_unknown_state_and_failed_model_reject_prediction(self):
        with self.assertRaises(s.EvidenceError):s.predict_stage(dict(passed=False),{},1,[0,0,10,10],5)
        with self.assertRaises(s.EvidenceError):s.predict_stage(dict(passed=True,signature={"isp":128}),dict(signature={"isp":256}),1,[0,0,10,10],5)

    def test_full_roi_estimator_inverts_before_averaging(self):
        response=np.repeat((np.arange(256,dtype=float)+1)[:,None]**2,3,axis=1)
        image=np.array([[[40]*3,[200]*3]],dtype=np.uint8)
        log,mask=s.log_radiance(image,1,response,[[4,250]]*3)
        self.assertTrue(mask.all())
        self.assertGreater(np.exp2(log).mean(),response[120,0])

    def test_native_failed_capture_cannot_be_model_input(self):
        fixture=ROOT / "build/worktrees/camera-deep-probe/build/exposure-sweep-v2-1"
        self.assertTrue(fixture.is_dir(),"run actual C sweep failure tests first")
        with self.assertRaisesRegex(s.EvidenceError,"incomplete sweep"):
            s.verify(fixture)

    def test_native_complete_simulated_capture_has_all_boundary_proof(self):
        # This is the real C producer with an explicitly simulated clock/sensor.
        result=s.verify(ROOT/"build/worktrees/camera-deep-probe/build/exposure-sweep-v2-0")
        self.assertEqual(len(result["stages"]),277)
        self.assertAlmostEqual(result["period_seconds"],.2)
        self.assertFalse(result["optical_time_verified"])

    def test_short_diagnostic_cannot_masquerade_as_full_calibration(self):
        fixture=ROOT/"build/worktrees/camera-deep-probe/build/exposure-sweep-v2-4"
        self.assertTrue(fixture.is_dir(),"run actual C diagnostic scope tests first")
        with self.assertRaisesRegex(s.EvidenceError,"unknown or changed sweep contract"):
            s.verify(fixture)

    def test_unknown_source_and_fake_complete_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            (p/"manifest.txt").write_text("SWEEP_CONTRACT source=unregistered\nSWEEP_END complete=1 stages=278 restored=1\n")
            with self.assertRaisesRegex(s.EvidenceError,"unknown or changed"):
                s.verify(p)

    def test_reusing_training_capture_is_not_cross_light_validation(self):
        with self.assertRaisesRegex(s.EvidenceError,"training batch"):
            s.compare(dict(capture_id="same"),{},dict(capture_id="same"),{})

    def test_output_never_overwrites_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/"evidence.json";p.write_text("original")
            with self.assertRaises(FileExistsError):s.write_new(p,{})
            self.assertEqual(p.read_text(),"original")


if __name__=="__main__":unittest.main(verbosity=2)
