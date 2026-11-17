"""Real record integrity, chart sampling and calibration solver boundaries."""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
# This test file shares the tool basename, so load the tool by explicit path.
spec=importlib.util.spec_from_file_location('chart_tool',ROOT/'tools/display_chart_calibrate.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
from camera_runtime_contract import CONTRACT_MATRIX

class ChartTests(unittest.TestCase):
    def test_contract_no_admission_and_known_downstream(self):
        rule=CONTRACT_MATRIX[m.SOURCE]
        self.assertTrue(rule['compute']);self.assertFalse(rule['lane_admission']);self.assertIsNone(rule['override'])
        self.assertIn(rule['downstream'],CONTRACT_MATRIX)

    def test_crc_truncation_and_invalid_low_bytes_fail_closed(self):
        h=bytearray(304);h[:4]=b'F3SR';struct.pack_into('<6I',h,4,1,304,614400,640,480,4)
        wire=bytes([42,2])*307200
        def record(w):
            struct.pack_into('<I',h,300,zlib.crc32(w,zlib.crc32(h[:300])))
            return bytes(h)+w
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'test.F3S';p.write_bytes(record(wire));payload,raw=m.read_raw(p)
            self.assertEqual(payload,wire);self.assertTrue((raw==170).all())
            b=bytearray(p.read_bytes());b[-2]^=1;p.write_bytes(b)
            with self.assertRaisesRegex(ValueError,'CRC'):m.read_raw(p)
            p.write_bytes(record(wire[:-1]+b'\x04'))
            with self.assertRaisesRegex(ValueError,'low byte'):m.read_raw(p)
            p.write_bytes(record(wire)[:-1])
            with self.assertRaises(ValueError):m.read_raw(p)

    def test_RGB16_TIFF_retains_low_bits_and_ICC(self):
        from PIL import Image,ImageCms
        icc=ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'out.tif';a=np.array([[[.10001,.50001,.90001],[.10002,.50002,.90002]]])
            m.tiff16(p,a,icc)
            with Image.open(p) as im:
                self.assertEqual(tuple(im.tag_v2[258]),(16,16,16))
                self.assertEqual(im.info['icc_profile'],icc)
                offset=im.tag_v2[273][0]
            pixels=np.frombuffer(p.read_bytes()[offset:],dtype='<u2').reshape(a.shape)
            np.testing.assert_array_equal(pixels,np.rint(a*65535).astype(np.uint16))
            self.assertNotEqual(pixels[0,0,0],pixels[0,1,0])

    def test_affine_fit_on_independent_samples_and_transfer_roundtrip(self):
        rng=np.random.default_rng(64);x=rng.uniform(.1,.7,(80,3));truth=np.array([[1.2,.1,0],[0,.8,.1],[.1,0,1.1],[-.03,-.02,-.01]])
        y=np.column_stack([x,np.ones(len(x))])@truth
        fit=m.fit(x[:24],y[:24]);pred=np.column_stack([x[24:],np.ones(56)])@fit
        self.assertLess(np.abs(pred-y[24:]).max(),.002)
        v=np.linspace(0,3,1000);np.testing.assert_allclose(m.decode(m.encode(v)),v,atol=1e-12)

    def test_defect_candidates_respect_phase_and_ignore_chart_edges(self):
        raw=np.full((64,64),200,dtype=np.uint16);raw[:,32:]=700
        raw[20,20]=0;raw[22,20]=0;raw[42,44]=1023
        self.assertEqual(set(m.bad_pixels(raw)),{(20,20),(20,22),(44,42)})
        image=np.zeros((80,120,3));image[:40,:60]=[.1,.2,.3];image[:40,60:]=[.4,.5,.6]
        samples,_=m.patch_samples(image,[[0,0],[119,0],[119,79],[0,79]],2,2)
        np.testing.assert_allclose(samples[0],[.1,.2,.3])
        with self.assertRaises(ValueError):m.patch_samples(image,[[-100,0],[119,0],[119,79],[-100,79]],2,2)

if __name__=='__main__':unittest.main()
