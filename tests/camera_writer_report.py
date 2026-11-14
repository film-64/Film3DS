"""Real C window fixtures joined to explicit synthetic observer test streams."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import camera_writer_report as report
import camera_writer_trace as trace


class ReportTests(unittest.TestCase):
    def fixture(self,scenario=0):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup);p=Path(temp.name);recovered=p/'recovered';recovered.mkdir();observer=p/'trace';observer.mkdir()
        source=ROOT/f'build/worktrees/camera-deep-probe/build/writer-window-{scenario}'
        for path in source.iterdir():shutil.copy(path,recovered/path.name)
        with (recovered/'manifest.txt').open('a') as f:f.write(f'END complete={int(scenario==0)} aborted=0 io_error=0\n')
        receipts=[]
        for f in recovered.iterdir():
            data=f.read_bytes();receipts.append(dict(name=f.name,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),crc32=f'{zlib.crc32(data):08x}'))
        (recovered/'received-ram-sha256.json').write_text(json.dumps(receipts))
        expected=json.loads((ROOT/'build/hardware/exposure-sweep/writer-audit/expected-camera-identity.json').read_text())
        text=(ROOT/'build/hardware/exposure-deep/run-01/final/driver-camera-startup-00100000.bin').read_bytes()
        (observer/'camera-text.bin').write_bytes(text);(observer/'expected-camera-identity.json').write_text(json.dumps(expected))
        status=dict(source='camera-writer-trace/v2' if scenario==4 else 'camera-writer-trace/v1',complete=True,identity_matched=True,detached=True,catch_disabled=True,gate_released=True,passed=True,
                    text_sha256=expected['text_sha256'],i2c_handle=0x50009,stops=8,entries=4,breakpoints=0,memory_writes=0)
        events=[dict(kind='gate_ready'),dict(kind='identity',i2c_handle=0x50009,sha256=expected['text_sha256']),dict(kind='gate_GO')]
        for i,address in enumerate((0x32d4,0x32d6,0x32d8,0x32da),1):events+=self.entry(i,address,128)
        if scenario==4:
            with (recovered/'manifest.txt').open('a') as f:f.write('END complete=1 aborted=0 io_error=0\n')
            text=(recovered/'manifest.txt').read_text().replace('END complete=0 aborted=0 io_error=0\n','');(recovered/'manifest.txt').write_text(text)
            for e in receipts:
                if e['name']=='manifest.txt':
                    b=(recovered/'manifest.txt').read_bytes();e.update(bytes=len(b),sha256=hashlib.sha256(b).hexdigest(),crc32=f'{zlib.crc32(b):08x}')
            (recovered/'received-ram-sha256.json').write_text(json.dumps(receipts))
            for e in events:
                if e['kind']=='i2c_return':e['register_r0']=e.pop('transport_result');e['service_result']=0
        events.extend([dict(kind='gate_DONE',line='FILMTRACE1 DONE ok=1\n'),dict(kind='gate_RELEASE')])
        (observer/'trace-status.json').write_text(json.dumps(status));self.events(p,events);return p
    def entry(self,i,address,value):
        words=[(8<<16)|(3<<6)|2,address,value,1,(1<<14)|2,0x8000000]+[0]*58
        r=dict(kind='i2c_entry',entry_id=i,thread='p17.1',handle=0x50009,pc=0x10bf90,lr=0x10fa3c,sp=0x0fffe000,
               words=words,buffers=[dict(index=0,pointer=0x8000000,bytes=1,hex='02')],stack_hex='00'*128)
        decoded,covered=trace.writes(r);r.update(writes=decoded,covered=covered)
        def stop(entry):
            values={'0':0x50009 if entry else 0,'d':r['sp'],'e':r['lr'],'f':r['pc']}
            packet='T05thread:p17.1;'+''.join(k+':'+struct.pack('<I',v).hex()+';' for k,v in values.items())+('syscall_entry' if entry else 'syscall_return')+':32;'
            return dict(kind='stop',packet=packet)
        response=dict(kind='i2c_return',entry_id=i,thread='p17.1',transport_result=0,words=[0x80040,0]+[0]*62)
        return [stop(True),r,stop(False),response]
    def events(self,p,events):
        with (p/'trace/trace.jsonl').open('w') as f:
            for r in events:f.write(json.dumps(r)+'\n')
    def load(self,p):return [json.loads(line) for line in (p/'trace/trace.jsonl').read_text().splitlines()]
    def test_no_camera_write_claim_requires_actual_positive_control(self):
        r=report.analyse(self.fixture());self.assertTrue(r['passed'],r['errors'])
        self.assertEqual(r['conclusion'],'IFP_changed_without_Camera_outbound_IFP_write')
        self.assertFalse(r['all_external_I2C_writers_excluded']);self.assertFalse(r['calibration_validated'])
    def test_external_IFP_write_is_preserved_with_caller(self):
        p=self.fixture();events=self.load(p);index=next(i for i,e in enumerate(events) if e['kind']=='gate_DONE');events[index:index]=self.entry(5,0x32d4,133);self.events(p,events)
        s=p/'trace/trace-status.json';r=json.loads(s.read_text());r.update(stops=10,entries=5);s.write_text(json.dumps(r))
        r=report.analyse(p);self.assertTrue(r['passed'],r['errors']);self.assertEqual(r['conclusion'],'Camera_outbound_IFP_write_observed')
        self.assertEqual(r['IFP_writes'][-1]['value'],133)
    def test_gate_failures_never_become_valid_measurements(self):
        for n in (1,2):self.assertFalse(report.analyse(self.fixture(n))['passed'])
    def test_missing_positive_control_and_unpaired_response_reject(self):
        for kind in ('i2c_entry','i2c_return'):
            p=self.fixture();events=self.load(p);events.remove(next(e for e in events if e['kind']==kind));self.events(p,events)
            r=report.analyse(p);self.assertFalse(r['passed']);self.assertEqual(r['conclusion'],'inconclusive')
    def test_fake_stop_wrong_handle_or_misplaced_proof_reject(self):
        for mode in ('handle','stop','misplaced'):
            p=self.fixture();events=self.load(p);entry=next(e for e in events if e['kind']=='i2c_entry')
            if mode=='handle':entry['handle']=999
            elif mode=='stop':events.remove(next(e for e in events if e['kind']=='stop'))
            else:events.remove(entry);events.append(entry)
            self.events(p,events);self.assertFalse(report.analyse(p)['passed'])
    def test_unknown_command_prevents_negative_inference(self):
        p=self.fixture();events=self.load(p);entry=next(e for e in events if e['kind']=='i2c_entry')
        entry['words'][0]=(6<<16)|(3<<6)|2;entry['writes']=[];entry['covered']=False;self.events(p,events)
        r=report.analyse(p);self.assertFalse(r['passed']);self.assertIn(6,r['unknown_I2C_commands'])
    def test_trace_failure_identity_mismatch_and_raw_integrity_reject(self):
        p=self.fixture();status=p/'trace/trace-status.json';s=json.loads(status.read_text());s['detached']=False;status.write_text(json.dumps(s));self.assertFalse(report.analyse(p)['passed'])
        p=self.fixture();(p/'trace/camera-text.bin').write_bytes(b'changed');self.assertFalse(report.analyse(p)['passed'])
        p=self.fixture()
        with (p/'recovered/reads.csv').open('a') as f:f.write('tamper')
        with self.assertRaisesRegex(ValueError,'integrity'):report.analyse(p)
    def test_instrumented_v2_observes133_but_v1_still_rejects_same_drift(self):
        r=report.analyse(self.fixture(4));self.assertTrue(r['passed'],r['errors']);self.assertFalse(r['calibration_validated'])
        self.assertEqual(r['groups'][2]['values']['32d4'],133)
        self.assertFalse(report.analyse(self.fixture(3))['passed'])


if __name__=='__main__':unittest.main()
