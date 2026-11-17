"""Actual C production scene pipeline on the two frozen user RAW originals."""
import hashlib,json,subprocess,sys,tempfile,unittest
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from display_chart_calibrate import decode,patch_samples,errors
from raw_scene_validate import compare
from camera_runtime_contract import CONTRACT_MATRIX

class SceneTests(unittest.TestCase):
    def test_actual_C_calibrated_charts_and_immutable_sources(self):
        target=json.loads((ROOT/'tests/fixtures/raw-scene-targets.json').read_text())
        for case in ('color','gray'):
            source=ROOT/f'romfs/rawqa/{case}.F3S';before=hashlib.sha256(source.read_bytes()).digest()
            with tempfile.TemporaryDirectory() as d:
                out=Path(d)/case
                cmd=[str(ROOT/'build/filmscene'),str(source),str(ROOT/f'romfs/calibration/display-{case}.f3isp'),str(out),'0','0','0','0','0','0','1.5']
                r=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
                report=json.loads((out/'report.json').read_text());self.assertEqual(report['source'],'film-raw-scene-probe/v1')
                samples=np.fromfile(out/'samples.f32',dtype='<f4');self.assertTrue(np.isfinite(samples).all());self.assertTrue((samples>1).any());self.assertTrue((samples<0).any())
                canvas=np.fromfile(out/'canvas.rgb16',dtype='<u2').reshape(480,640,3)/65535
                values,_=patch_samples(decode(canvas),target['layout']['raw_'+case],4 if case=='color' else 1,6 if case=='color' else 21)
                if case=='color':self.assertLess(errors(values,np.array(target['color_linear_srgb']))['mean_deltaE76'],3)
                else:self.assertTrue((np.diff(values@np.array([.2126,.7152,.0722]))>0).all())
                self.assertTrue(compare(out,out)['passed'])
                # Output collisions reject rather than replace a completed result.
                r=subprocess.run(cmd,capture_output=True,text=True);self.assertNotEqual(r.returncode,0)
            self.assertEqual(hashlib.sha256(source.read_bytes()).digest(),before)

    def test_scene_sources_are_registered_without_admission_escape(self):
        for name in ('film-raw-scene/v1','film-raw-scene-grade/v1','film-raw-scene-probe/v1'):
            rule=CONTRACT_MATRIX[name];self.assertTrue(rule['compute']);self.assertFalse(rule['lane_admission']);self.assertIsNone(rule['override'])

if __name__=='__main__':unittest.main()
