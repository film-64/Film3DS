"""Smoke test: raw source-bound paired calibration evidence and 21-patch QA."""
from pathlib import Path
import hashlib
import importlib.util
import struct
import tempfile
import unittest
import zlib
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("gray_pair_report",ROOT/"tools/gray_pair_report.py")
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class GrayPairReportTest(unittest.TestCase):
    def test_valid_pair_and_tampering(self):
        with tempfile.TemporaryDirectory() as directory:
            d=Path(directory)
            raw=np.full((480,640),192,dtype="<u2")
            payload=raw.tobytes()
            packed=np.empty((480,640,2),dtype=np.uint8)
            packed[:,:,0]=(raw>>2).astype(np.uint8)
            packed[:,:,1]=(raw&3).astype(np.uint8)
            crc=zlib.crc32(packed.tobytes(),zlib.crc32(struct.pack("<III",640,480,4)))
            ifd=bytearray(2048);struct.pack_into("<4sI",ifd,0,b"II*\0",8)
            tags=[(256,4,640),(257,4,480),(258,3,16),(259,3,1),
                  (262,3,32803),(273,4,2048),(277,3,1),(278,4,480),
                  (279,4,614400),(50972,1,512)]
            struct.pack_into("<H",ifd,8,len(tags))
            for i,(tag,kind,value) in enumerate(tags):
                struct.pack_into("<HHII",ifd,10+i*12,tag,kind,16 if tag==50972 else 1,value)
            ifd[512:528]=hashlib.md5(payload).digest()
            dng=d/"ROLL0001.DNG";dng.write_bytes(ifd+payload)
            hdr=bytearray(64);hdr[:4]=b"F3YG"
            struct.pack_into("<14I",hdr,4,1,1,crc,640,480,0,1,0,1,1,12345,0,614400,0)
            yuv=bytes([128,128,128,128])*(640*480//2)
            struct.pack_into("<I",hdr,56,zlib.crc32(yuv))
            struct.pack_into("<I",hdr,60,zlib.crc32(hdr[:60]))
            f3y=d/"ROLL0001.F3Y";f3y.write_bytes(hdr+yuv)
            result=m.analyze(dng,f3y,ROOT/"romfs/calibration/display-gray.f3isp",
                             (0,210,640,290),(0,210,640,290))
            self.assertEqual(result["record"]["source_crc"],crc)
            self.assertEqual(len(result["patches"]),21)
            self.assertEqual(result["record"]["native_level"],1)
            self.assertTrue(result["record"]["auto_exposure"])
            tampered=bytearray(f3y.read_bytes());tampered[-1]^=0xff;f3y.write_bytes(tampered)
            with self.assertRaisesRegex(ValueError,"payload CRC"):m.read_f3y(f3y)
            # The new sidecar records the actual camera ROI; v1 remains readable.
            h2=bytearray(80)
            h2[:60]=hdr[:60]
            struct.pack_into("<I",h2,4,2)
            struct.pack_into("<4I",h2,60,160,120,320,240)
            struct.pack_into("<I",h2,76,zlib.crc32(h2[:76]))
            f3y.write_bytes(h2+yuv)
            meta,_=m.read_f3y(f3y)
            self.assertEqual(meta["version"],2)
            self.assertEqual(tuple(meta["ae_window"]),(160,120,320,240))
            corrupt=bytearray(h2+yuv)
            struct.pack_into("<I",corrupt,68,319)
            struct.pack_into("<I",corrupt,76,zlib.crc32(corrupt[:76]))
            f3y.write_bytes(corrupt)
            with self.assertRaisesRegex(ValueError,"AE window"):m.read_f3y(f3y)
            f3y.write_bytes(h2+yuv)
            bad=bytearray(dng.read_bytes());bad[-1]^=1;dng.write_bytes(bad)
            with self.assertRaisesRegex(ValueError,"digest"):m.read_dng(dng)
if __name__=="__main__":
    unittest.main()
