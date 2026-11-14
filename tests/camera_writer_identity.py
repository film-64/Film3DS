"""Identity failure must detach; archived addresses cannot bypass a mismatch."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import camera_writer_identity as identity


class Debugger:
    def __init__(self,data,mode='ok'):self.data=data;self.mode=mode;self.commands=[]
    def command(self,cmd,timeout):
        self.commands.append(cmd)
        if 'info os processes' in cmd:
            names='23 camera\n' if self.mode!='wrong_process' else '23 not_camera\n'
            return ['~'+json.dumps(names),'1^done']
        if cmd.startswith('-data-read-memory'):
            if self.mode=='read_error':raise OSError('injected disconnected read')
            _,address,size=cmd.split();offset=int(address,16)-0x100000;data=self.data[offset:offset+int(size)]
            return ['1^done,memory=[{contents="'+data.hex()+'"}]']
        return ['1^done']
    def stop(self,answer,timeout):pass


class IdentityTests(unittest.TestCase):
    def exercise(self,mode='ok',source=identity.SOURCE):
        data=bytes(range(256))*336
        expected=dict(base='00100000',text_bytes=len(data),text_sha256=hashlib.sha256(data).hexdigest(),entry_bytes=dict(write=dict(address='0010899c',bytes=data[0x899c:0x899c+16].hex())))
        if mode=='mismatch':data=b'X'+data[1:]
        debugger=Debugger(data,mode);self.debugger=debugger
        with tempfile.TemporaryDirectory() as temp:
            return identity.identify(debugger,expected,Path(temp)/'run','test',source=source)
    def test_complete_text_and_signature_match(self):
        r=self.exercise();self.assertTrue(r['passed']);self.assertTrue(r['detached']);self.assertEqual(r['captured_bytes'],0x15000)
        self.assertFalse(any('break' in c or 'write' in c or 'continue' in c for c in self.debugger.commands))
    def test_text_mismatch_cannot_enable_trace(self):
        r=self.exercise('mismatch');self.assertFalse(r['passed']);self.assertTrue(r['detached'])
    def test_read_failure_still_attempts_detach_once(self):
        with self.assertRaises(OSError):self.exercise('read_error')
        self.assertEqual(self.debugger.commands.count('-target-detach'),1)
        self.assertEqual(sum(c.startswith('-target-attach') for c in self.debugger.commands),1)
    def test_wrong_process_never_attaches(self):
        with self.assertRaisesRegex(ValueError,'identity'):self.exercise('wrong_process')
        self.assertFalse(any(c.startswith('-target-attach') for c in self.debugger.commands))
    def test_unknown_source_fails_before_network(self):
        with self.assertRaisesRegex(ValueError,'unregistered'):self.exercise(source='unknown')
        self.assertFalse(self.debugger.commands)


if __name__=='__main__':unittest.main()
