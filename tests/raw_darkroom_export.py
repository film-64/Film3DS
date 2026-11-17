"""Run the actual C RAW/ISP/LUT/grain exporter; no camera or broad mocks."""
from pathlib import Path
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from profile import encode
from camera_runtime_contract import CONTRACT_MATRIX,RUNTIME_INTERNAL_FIELDS,header


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.dir=Path(self.temp.name)
        values=[100*(1+((0,1,1,2)[(y&1)*2+(x&1)]))+((x*23+y*31)%80) for y in range(12) for x in range(16)]
        self.payload=bytes(v for n in values for v in (n>>2,n&3))
        self.raw=self.dir/'input.bin';self.raw.write_bytes(self.payload)
        self.params=self.dir/'params.txt'
        self.profile=self.dir/'profile.f3p';self.profile.write_bytes(encode(dict(input='srgb',output='srgb')))
        self.lut=self.dir/'identity.cube'
        with self.lut.open('w') as f:
            f.write('LUT_3D_SIZE 17\n')
            for b in range(17):
                for g in range(17):
                    for r in range(17):f.write(f'{r/16} {g/16} {b/16}\n')
        self.grain=ROOT/'romfs/grain/medium.f3g'

    def tearDown(self):self.temp.cleanup()

    def run_export(self,name,extra='',lut=True,expected=0,texture=None):
        self.params.write_text('F3RAW1\nwidth 16\nheight 12\nshoulder 0\ndenoise 0\n'+extra)
        out=self.dir/name
        command=[str(ROOT/'build/filmraw'),str(self.raw),str(self.params),str(out),str(self.grain)]
        if lut:command.extend([str(self.lut),str(self.profile)])
        if texture:command.append(str(texture))
        result=subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(result.returncode,expected,result.stderr)
        self.assertEqual(self.raw.read_bytes(),self.payload)
        return out

    def pixels(self,path):
        with Image.open(path) as im:im.load();self.assertEqual(im.size,(16,12));return list(im.getdata())

    def test_same_prefix_identity_LUT_grain_zero_and_no_overwrite(self):
        out=self.run_export('identity','grain_amount 0\n')
        plain=self.pixels(out/'plain.ppm');look=self.pixels(out/'look.ppm')
        self.assertLessEqual(max(abs(a-b) for p,q in zip(plain,look) for a,b in zip(p,q)),1)
        self.assertEqual((out/'plain.ppm').read_bytes(),(out/'plain-grain.ppm').read_bytes())
        self.assertEqual((out/'look.ppm').read_bytes(),(out/'look-grain.ppm').read_bytes())
        self.assertEqual(json.loads((out/'completed.json').read_text())['isp_preparations'],1)
        self.assertIn('isp_preparations',RUNTIME_INTERNAL_FIELDS)
        before={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir()}
        self.run_export('identity','grain_amount 0\n',expected=3)
        self.assertEqual(before,{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir()})

    def test_exposure_WB_CCM_tone_sharpness_and_seed_controls(self):
        base=self.run_export('base','grain_amount 0\n')
        base_pixels=self.pixels(base/'plain.ppm')
        means=[]
        for ev in (-1,0,1):
            out=self.run_export('ev'+str(ev),f'exposure_ev {ev}\ngrain_amount 0\n')
            means.append(sum(sum(p) for p in self.pixels(out/'plain.ppm')))
        self.assertLess(means[0],means[1]);self.assertLess(means[1],means[2])
        for name,value in [('wb','2 1 1'),('ccm','1.5 0 0 0 1 0 0 0 1'),('highlights','.5'),
                           ('midtones','.5'),('shadows','.5'),('saturation','.5'),('sharpness','2'),('ccm_offset','.05 0 0'),('local_strength','.5'),('shading_r','0 0 0 1 0 0')]:
            out=self.run_export(name,f'{name} {value}\ngrain_amount 0\n')
            self.assertNotEqual(self.pixels(out/'plain.ppm'),base_pixels,name)
        a=self.run_export('grain1','grain_amount .5\nseed 64\n')
        b=self.run_export('grain2','grain_amount .5\nseed 64\n')
        c=self.run_export('grain3','grain_amount .5\nseed 65\n')
        self.assertEqual((a/'plain-grain.ppm').read_bytes(),(b/'plain-grain.ppm').read_bytes())
        self.assertNotEqual((a/'plain.ppm').read_bytes(),(a/'plain-grain.ppm').read_bytes())
        self.assertNotEqual((a/'plain-grain.ppm').read_bytes(),(c/'plain-grain.ppm').read_bytes())

    def test_bad_parameters_unknown_runtime_fields_and_corrupt_transport(self):
        self.run_export('duplicate','width 16\n',expected=1)
        for i,key in enumerate(RUNTIME_INTERNAL_FIELDS):
            out=self.run_export('internal'+str(i),f'{key} 0\n',expected=1)
            self.assertFalse(out.exists())
        self.run_export('fake','admission_decision admitted\n',expected=1)
        self.run_export('nan','wb nan 1 1\n',expected=1)
        self.run_export('invalid-local','local_strength 1.1\n',expected=1)
        self.run_export('invalid-shading','shading_r 0 0 0 0 0 5\n',expected=1)
        out=self.run_export('evbad','exposure_ev 9\n',expected=1)
        self.assertFalse((out/'completed.json').exists())
        bad=bytearray(self.payload);bad[-1]=4;self.payload=bytes(bad);self.raw.write_bytes(self.payload)
        out=self.run_export('badraw',expected=5)
        self.assertFalse((out/'completed.json').exists())

    def test_real_nonidentity_LUT_and_plain_only_mode(self):
        self.lut.write_text('LUT_3D_SIZE 17\n'+'.1 .2 .3\n'*(17**3))
        out=self.run_export('constant','grain_amount 0\n')
        self.assertNotEqual((out/'plain.ppm').read_bytes(),(out/'look.ppm').read_bytes())
        out=self.run_export('plainonly','grain_amount 0\n',lut=False)
        self.assertFalse((out/'look.jpg').exists());self.assertTrue((out/'plain.jpg').exists())

    def test_contracts_and_frozen_header_remain_fail_closed(self):
        self.assertEqual((ROOT/'include/film_runtime_contract.h').read_text(),header())
        keys=re.search(r'keys\[\]=\{(.*?)\};',(ROOT/'tools/raw_darkroom_host.c').read_text(),re.S).group(1)
        self.assertEqual(tuple(re.findall(r'"([^"]+)"',keys)),CONTRACT_MATRIX['film-raw-darkroom-export/v1']['parameter_fields'])
        self.assertFalse(set(CONTRACT_MATRIX['film-raw-darkroom-export/v1']['parameter_fields'])&set(RUNTIME_INTERNAL_FIELDS))
        for name in ('film-raw10-unpack/v1','film-raw-isp/v1','film-raw-grade/v1','film-grain-layered/v1'):
            rule=CONTRACT_MATRIX[name];self.assertTrue(rule['compute']);self.assertFalse(rule['lane_admission']);self.assertIsNone(rule['override'])

    def test_actual_texture_loader_irregular_sampling_and_CRC_rejection(self):
        rng=np.random.default_rng(64);tiles=[]
        for i in range(9):
            tile=rng.normal(size=(128,128));tile=(tile-tile.mean())/tile.std()
            tiles.append(np.rint(tile*4096).astype('<i2'))
        payload=struct.pack('<4s4I9f',b'F3GT',1,128,128,9,*range(16,241,28))+np.stack(tiles).tobytes()
        payload+=struct.pack('<I',zlib.crc32(payload))
        atlas=self.dir/'texture.f3gt';atlas.write_bytes(payload)
        a=self.run_export('textured1','grain_amount .5\nseed 61\n',texture=atlas)
        b=self.run_export('textured2','grain_amount .5\nseed 61\n',texture=atlas)
        self.assertEqual((a/'plain-grain.ppm').read_bytes(),(b/'plain-grain.ppm').read_bytes())
        self.assertNotEqual((a/'plain-grain.ppm').read_bytes(),(a/'plain.ppm').read_bytes())
        self.assertTrue(json.loads((a/'completed.json').read_text())['texture_atlas'])
        self.run_export('texture-no-lut','grain_amount 0\n',texture=atlas,lut=False)
        bad=bytearray(payload);bad[200]^=1;atlas.write_bytes(bad)
        out=self.run_export('corrupt-atlas',texture=atlas,expected=5)
        self.assertFalse(out.exists())

    def test_grain_response_size_and_cluster_controls(self):
        base=self.run_export('gbase','grain_amount .8\nseed 7\n')
        pixels=(base/'plain-grain.ppm').read_bytes()
        for name,value in [('grain_size','2'),('grain_coarse','1'),('grain_shadow','0'),
                           ('grain_mid','0'),('grain_highlight','0')]:
            out=self.run_export(name,f'grain_amount .8\nseed 7\n{name} {value}\n')
            self.assertNotEqual(pixels,(out/'plain-grain.ppm').read_bytes(),name)
        out=self.run_export('no-response','grain_amount 1\ngrain_shadow 0\ngrain_mid 0\ngrain_highlight 0\n')
        self.assertEqual((out/'plain.ppm').read_bytes(),(out/'plain-grain.ppm').read_bytes())
        out=self.run_export('invalid-grain','grain_size 0\n',expected=1)
        self.assertFalse((out/'completed.json').exists())

    def test_private_reference_texture_build_matches_profile(self):
        reference=ROOT/'build/hardware/dxo-grain/DxO-Pan400.tif'
        if not reference.exists():self.skipTest('private grain reference not present')
        from build_grain_texture import build
        atlas=self.dir/'pan400.f3gt'
        r=build(reference,ROOT/'romfs/grain/medium.f3g',atlas)
        self.assertEqual(len(r['bands']),9)
        self.run_export('real-texture','grain_amount .5\n',texture=atlas)
        with self.assertRaises(FileExistsError):build(reference,ROOT/'romfs/grain/medium.f3g',atlas)
        with self.assertRaises(ValueError):build(reference,ROOT/'romfs/grain/fine.f3g',self.dir/'wrong.f3gt')


if __name__=='__main__':unittest.main()
