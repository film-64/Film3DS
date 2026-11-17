import sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import exposure_response_fit as f

class FitTests(unittest.TestCase):
    def synthetic(self):
        t=f.np.array([4,8,16,32,64,128,256,528,1024,1585],float)/1000
        light=f.np.linspace(.015,.6,48*64).reshape(48,64,1)*f.np.array([.85,1.,.7])
        return t,[f.np.rint(f.np.clip(light*x,0,1)**(1/2.2)*255).astype(f.np.uint8) for x in t]
    def test_nonlinear_response_and_fixed_holdout(self):
        t,im=self.synthetic();r=f.fit_profile(im,t);self.assertTrue(r['relative_response_passed'],r)
        self.assertTrue(set(r['training_indices']).isdisjoint(r['holdout_indices']));self.assertFalse(r['optical_time_verified'])
    def test_heldout_corruption_cannot_train_itself_away(self):
        t,im=self.synthetic();im[5]=f.np.rint(f.np.clip(im[5].astype(float)*1.3,0,255)).astype(f.np.uint8);r=f.fit_profile(im,t);self.assertFalse(r['relative_response_passed'])
    def test_missing_effective_levels_cannot_make_model(self):
        t,im=self.synthetic();im=im[:8]
        for i in (0,2):im[i]=f.np.zeros_like(im[i])
        r=f.fit_profile(im,t[:8]);self.assertFalse(r['relative_response_passed']);self.assertEqual(r['reason'],'insufficient_effective_training_exposures')
    def test_training_plateau_is_not_normal_exposure(self):
        t,im=self.synthetic();im[8]=f.np.full_like(im[8],180);im[9]=f.np.full_like(im[9],180);r=f.fit_profile(im,t)
        self.assertEqual({x['index'] for x in r['training_plateau_masks']},{9});self.assertTrue(all(9 not in x for x in r['effective_training_indices']))
    def test_dark_saturated_and_constant_data_do_not_get_fallback(self):
        t,im=self.synthetic()
        for value in (0,255,100):
            r=f.fit_profile([f.np.full_like(x,value) for x in im],t);self.assertFalse(r['relative_response_passed']);self.assertNotIn('log2_response',r)
    def test_invalid_grid_or_time_rejected(self):
        t,im=self.synthetic()
        with self.assertRaises(ValueError):f.fit_profile(im[:1],t[:1])
        for value in (0,-1,f.np.inf,f.np.nan):
            bad=t.copy();bad[2]=value
            with self.assertRaises(ValueError):f.fit_profile(im,bad)

if __name__=='__main__':unittest.main()
