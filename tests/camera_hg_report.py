"""Actual C fixtures plus boundary tampering; no synthetic admitted traces."""
import csv
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zlib
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import camera_hg_report as report

class HGTests(unittest.TestCase):
    def receipts(self,out):
        entries=[]
        for p in sorted(out.iterdir()):
            if p.name=='received-ram-sha256.json':continue
            b=p.read_bytes();entries.append(dict(name=p.name,bytes=len(b),crc32=f'{zlib.crc32(b):08x}',sha256=hashlib.sha256(b).hexdigest()))
        (out/'received-ram-sha256.json').write_text(json.dumps(entries))
    def fixture(self,n=0):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup);out=Path(tmp.name)
        source=ROOT/f'build/worktrees/camera-deep-probe/build/hg-isolation-{n}'
        self.assertTrue(source.is_dir(),'run HG_ISOLATION_ONLY=1 ./build/probe-driver first')
        for p in source.iterdir():shutil.copyfile(p,out/p.name)
        with (out/'manifest.txt').open('a') as f:f.write(f'END complete={int(n<3)} aborted=0 io_error=0\n')
        self.receipts(out);return out
    def edit(self,out,file,fn):
        p=out/file
        with p.open() as f:r=csv.DictReader(f);fields=r.fieldnames;rows=list(r)
        fn(rows)
        with p.open('w') as f:w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
        self.receipts(out)
    def test_three_actual_producer_models(self):
        for n,conclusion in enumerate(('HG_gate_dependency_supported','HG_gate_not_sufficient_to_stop_update','no_overwrite_observed')):
            r=report.analyse(self.fixture(n));self.assertTrue(r['passed'],r['errors']);self.assertEqual(r['conclusion'],conclusion)
            self.assertFalse(r['calibration_validated']);self.assertFalse(r['writer_identified']);self.assertFalse(r['timing_cause_identified'])
    def test_producer_failures_cannot_pass(self):
        for n in range(3,8):
            r=report.analyse(self.fixture(n));self.assertFalse(r['passed'],n);self.assertEqual(r['conclusion'],'inconclusive')
    def test_extra_compute_and_configuration_calls_rejected(self):
        for op in ('SetReceiving','StartCapture','WriteMcuVariableI2c','SwitchContext','ClearBuffer'):
            out=self.fixture()
            def fn(rows):
                row=next(r for r in rows if r['phase']=='6' and r['operation']=='WaitVsync')
                rows.insert(rows.index(row),dict(row,operation=op,addr='a103',value='5'))
            self.edit(out,'calls.csv',fn);self.assertFalse(report.analyse(out)['passed'],op)
    def test_misplaced_gate_and_fixed_control_drift_rejected(self):
        for addr in ('a102','3012','a34e','a355','221f'):
            out=self.fixture()
            def fn(rows):
                r=next(r for r in rows if r['phase']=='6' and r['name']=='isp_update_state' and r['when']=='after_poll' and r['addr']==addr)
                r['value']=str(int(r['value'])^(32 if addr=='a355' else 1))
            self.edit(out,'reads.csv',fn);self.assertFalse(report.analyse(out)['passed'],addr)
    def test_snapshot_width_missing_and_failed_read_rejected(self):
        for action in ('width','missing','failed'):
            out=self.fixture()
            def fn(rows):
                r=next(r for r in rows if r['name']=='hg_snapshot' and r['addr']=='ab17')
                if action=='width':r['value']='256'
                elif action=='missing':rows.remove(r)
                else:r['result']='d900182f'
            self.edit(out,'reads.csv',fn);self.assertFalse(report.analyse(out)['passed'])
    def test_stale_VSYNC_wrong_handle_and_pending_DMA_rejected(self):
        for op,field,value in (('WaitVsync','value','42'),('CloseDMAEvent','value','42'),('IsFinishedReceiving','value','0'),('HGPhaseDelay','value','1')):
            out=self.fixture()
            self.edit(out,'calls.csv',lambda rows:next(r for r in rows if r['phase']=='5' and r['operation']==op).__setitem__(field,value))
            self.assertFalse(report.analyse(out)['passed'])
        out=self.fixture();p=out/'manifest.txt';p.write_text(p.read_text().replace('latest=1200','latest=1000'));self.receipts(out)
        self.assertFalse(report.analyse(out)['passed'])
    def test_poll_time_and_value_correspondence_rejected(self):
        out=self.fixture()
        self.edit(out,'reads.csv',lambda rows:next(r for r in rows if r['name']=='hg_red').__setitem__('value','129'))
        self.assertFalse(report.analyse(out)['passed'])
        out=self.fixture();p=out/'manifest.txt';s=p.read_text();p.write_text(s.replace('index=0 begin_ms=', 'index=99 begin_ms=',1));self.receipts(out)
        self.assertFalse(report.analyse(out)['passed'])
    def test_unknown_source_and_integrity_fail_closed(self):
        out=self.fixture();p=out/'manifest.txt';p.write_text(p.read_text().replace(report.HG_SOURCE,'unknown'));self.receipts(out)
        with self.assertRaisesRegex(ValueError,'unregistered'):report.analyse(out)
        with (out/'reads.csv').open('a') as f:f.write('tamper')
        with self.assertRaisesRegex(ValueError,'integrity'):report.analyse(out)

if __name__=='__main__':unittest.main()
