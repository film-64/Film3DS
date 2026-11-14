import sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import exposure_response_fit as f
from exposure_sweep_contract import DENSE_COARSE,EXPOSURE_DENSE_RESPONSE_SOURCE as SOURCE

class DenseFitTests(unittest.TestCase):
    def synthetic(self,fps=5):
        t=f.np.array(DENSE_COARSE[fps],float)/1000;light=f.np.linspace(.015,.6,48*64).reshape(48,64,1)*f.np.array([.85,1.,.7])
        return t,[f.np.rint(f.np.clip(light*x,0,1)**(1/2.2)*255).astype(f.np.uint8) for x in t]
    def test_both_profiles_fixed_alternating_holdout(self):
        for fps in (5,15):
            t,im=self.synthetic(fps);r=f.fit_profile(im,t,SOURCE);self.assertTrue(r['relative_response_passed'],r);self.assertEqual(r['training_indices'],list(range(0,len(t),2)));self.assertEqual(r['holdout_indices'],list(range(1,len(t),2)))
    def test_holdout_corruption_does_not_fit_itself(self):
        t,im=self.synthetic();im[5]=f.np.rint(f.np.clip(im[5].astype(float)*1.3,0,255)).astype(f.np.uint8);self.assertFalse(f.fit_profile(im,t,SOURCE)['relative_response_passed'])
    def test_plateau_masks_both_training_neighbors(self):
        t,im=self.synthetic();im[16]=f.np.full_like(im[16],180);im[18]=f.np.full_like(im[18],180);r=f.fit_profile(im,t,SOURCE);self.assertTrue(all(16 not in x and 18 not in x for x in r['effective_training_indices']))
    def test_no_fallback_for_black_or_insufficient_coverage(self):
        t,im=self.synthetic()
        for value in (0,255,100):self.assertFalse(f.fit_profile([f.np.full_like(x,value) for x in im],t,SOURCE)['relative_response_passed'])
    def test_unknown_source_or_grid_rejects(self):
        t,im=self.synthetic()
        with self.assertRaises(ValueError):f.fit_profile(im,t,'unknown')
        with self.assertRaises(ValueError):f.fit_profile(im[:10],t[:10],SOURCE)

if __name__=='__main__':unittest.main()
