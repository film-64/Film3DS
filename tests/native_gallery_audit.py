"""Test static conclusions against the actual private original, without mocks."""
from pathlib import Path
import json
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from native_gallery_audit import SOURCE,analyze,create,verify


class GalleryAuditTests(unittest.TestCase):
    def setUp(self):
        self.code=ROOT/'build/hardware/native-trace/cxi-jp/exefs/code.bin'
        if not self.code.exists():self.skipTest('private native code fixture not present')
        self.temp=tempfile.TemporaryDirectory()
        self.out=Path(self.temp.name)/'evidence.json'

    def tearDown(self):self.temp.cleanup()

    def test_actual_profile_RTTI_dispatch_cancel_and_release(self):
        result=create(self.code,self.out)
        self.assertEqual(verify(self.code,self.out),result)
        self.assertEqual(len(result['classes']),4)
        self.assertEqual(len(result['checked_edges']),13)
        self.assertFalse(result['limits']['hook_approved'])
        self.assertIn('002BB140',result['nodes']['mp_constructor_borrowed_output']['direct_call_sites'])
        self.assertIn('002BB254',result['nodes']['jpeg_constructor_borrowed_output']['direct_call_sites'])
        self.assertIn('002EF220',result['nodes']['load_release_by_id']['direct_call_sites'])
        self.assertIn('002EF09C',result['nodes']['encoded_buffer_lookup_by_key']['direct_call_sites'])
        with self.assertRaises(FileExistsError):create(self.code,self.out)

    def test_unknown_source_and_version_fail_before_output(self):
        for source in ('unknown','scheduler_decision:admitted','startup_warmup','film-native-jpeg-decode/v1'):
            with self.assertRaises(ValueError):create(self.code,self.out,source)
            self.assertFalse(self.out.exists())
        bad=Path(self.temp.name)/'changed-code'
        data=bytearray(self.code.read_bytes());data[0x1ffd54-0x100000]^=1;bad.write_bytes(data)
        with self.assertRaises(ValueError):create(bad,self.out)
        self.assertFalse(self.out.exists())

    def test_misplaced_proof_or_unsupported_hook_claim_is_rejected(self):
        result=create(self.code,self.out)
        for kind in ('proof','hook','edge'):
            changed=json.loads(json.dumps(result))
            if kind=='proof':changed['metadata']=changed.pop('local_trace')
            if kind=='hook':changed['limits']['hook_approved']=True
            if kind=='edge':changed['checked_edges'][0]['target']='00255520'
            self.out.write_text(json.dumps(changed))
            with self.assertRaises(ValueError):verify(self.code,self.out)


if __name__=='__main__':unittest.main()
