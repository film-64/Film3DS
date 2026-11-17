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
import camera_gain_matrix_report as report
from exposure_sweep_contract import GAIN_MATRIX_SOURCE as CCM_SOURCE

class GainMatrixTests(unittest.TestCase):
    def receipts(self,out):
        entries=[]
        for p in sorted(out.iterdir()):
            if p.name=='received-ram-sha256.json':continue
            b=p.read_bytes();entries.append(dict(name=p.name,bytes=len(b),crc32=f'{zlib.crc32(b):08x}',sha256=hashlib.sha256(b).hexdigest()))
        (out/'received-ram-sha256.json').write_text(json.dumps(entries))
    def fixture(self,n=0):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup);out=Path(tmp.name)
        source=ROOT/f'build/worktrees/camera-deep-probe/build/gain-matrix-v4-{n}'
        self.assertTrue(source.is_dir(),'run GAIN_MATRIX_ONLY=1 ./build/probe-driver first')
        for p in source.iterdir():shutil.copyfile(p,out/p.name)
        with (out/'manifest.txt').open('a') as f:f.write(f'END complete={int(n<3 or n in (9,11))} aborted=0 io_error=0\n')
        self.receipts(out);return out
    def edit(self,out,file,fn):
        p=out/file
        with p.open() as f:r=csv.DictReader(f);fields=r.fieldnames;rows=list(r)
        fn(rows)
        with p.open('w') as f:w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
        self.receipts(out)
    def test_three_actual_models(self):
        for n,expected in enumerate(([],[2,3,6,7],list(range(8)))):
            r=report.analyse(self.fixture(n));self.assertTrue(r['passed'],r['errors']);self.assertEqual(r['controllable_nodes'],expected)
            self.assertFalse(r['calibration_validated']);self.assertFalse(r['writer_identified'])
    def test_dynamic_CCM_is_explicit_observation(self):
        r=report.analyse(self.fixture(9));self.assertTrue(r['passed'],r['errors'])
        c=r['nodes'][0]['snapshots']['before_stream']['ccm_snapshot_coherence'];self.assertEqual(c['match_before'],'0');self.assertEqual(c['match_after'],'0');self.assertFalse(c['atomic'])
    def test_coherence_cannot_be_forged_or_static_proof_misplaced(self):
        out=self.fixture(9);p=out/'manifest.txt';p.write_text(p.read_text().replace('match_before=0 match_after=0','match_before=1 match_after=1',1));self.receipts(out);self.assertFalse(report.analyse(out)['passed'])
        out=self.fixture();self.edit(out,'reads.csv',lambda rows:next(r for r in rows if r['name']=='matrix_alias_hi').__setitem__('addr','a332'));self.assertFalse(report.analyse(out)['passed'])
    def test_initial_subject_overwrite_is_data_invalid_read_is_not(self):
        r=report.analyse(self.fixture(11));self.assertTrue(r['passed'],r['errors'])
        out=self.fixture();self.edit(out,'reads.csv',lambda rows:next(r for r in rows if r['name']=='matrix_initial_gain').__setitem__('value','0'));self.assertFalse(report.analyse(out)['passed'])
    def test_millisecond_equal_stop_boundary(self):
        out=self.fixture()
        with (out/'reads.csv').open() as f:rs=list(csv.DictReader(f))
        last=max(int(r['time_ms']) for r in rs if r['phase']=='5' and r['name']=='gain_matrix_state' and r['when']=='vsync')
        self.edit(out,'calls.csv',lambda rows:next(r for r in rows if r['phase']=='5' and r['operation']=='StopCapture').__setitem__('time_ms',str(last)));r=report.analyse(out);self.assertTrue(r['passed'],r['errors'])
        self.edit(out,'calls.csv',lambda rows:next(r for r in rows if r['phase']=='5' and r['operation']=='StopCapture').__setitem__('time_ms',str(last-1)));self.assertFalse(report.analyse(out)['passed'])
    def test_actual_producer_failures(self):
        for n in list(range(3,9))+[10,12]:
            r=report.analyse(self.fixture(n));self.assertFalse(r['passed'],n)
    def test_extra_work_rejected(self):
        for op in ('SetReceiving','StartCapture','WriteMcuVariableI2c','SwitchContext'):
            out=self.fixture()
            def fn(rows):
                r=next(r for r in rows if r['phase']=='5' and r['operation']=='WaitVsync');rows.insert(rows.index(r),dict(r,operation=op))
            self.edit(out,'calls.csv',fn);self.assertFalse(report.analyse(out)['passed'],op)
    def test_fixed_control_drift_rejected(self):
        for addr in ('a102','a355','3012','300a','221f','a34e'):
            out=self.fixture()
            self.edit(out,'reads.csv',lambda rows:next(r for r in rows if r['name']=='gain_matrix_state' and r['when']=='vsync' and r['addr']==addr).__setitem__('value','999'))
            self.assertFalse(report.analyse(out)['passed'],addr)
    def test_adaptive_branch_and_cycles_rejected(self):
        for field,value in (('reason','first_held'),('cycles','8'),('node','99')):
            out=self.fixture()
            self.edit(out,'matrix_plan.csv',lambda rows:rows[1].__setitem__(field,value));self.assertFalse(report.analyse(out)['passed'])
    def test_extra_initial_ROI_rejected(self):
        out=self.fixture();(out/'s1-p999-fake.yuyv').write_bytes(bytes(24576));self.receipts(out);self.assertFalse(report.analyse(out)['passed'])
    def test_event_corruption_rejected(self):
        for field,value in (('latest','0'),('period','99999'),('held','1'),('cycles','999'),('phase','99')):
            out=self.fixture()
            self.edit(out,'matrix_events.csv',lambda rows:next(r for r in rows if r['trial']=='0').__setitem__(field,value));self.assertFalse(report.analyse(out)['passed'],field)
    def test_byte_failure_and_alias_rejected(self):
        for name,field,value in (('ccm_byte','result','c9602fef'),('ccm_byte','value','256'),('matrix_alias_native_after','value','641')):
            out=self.fixture();self.edit(out,'reads.csv',lambda rows:next(r for r in rows if r['name']==name).__setitem__(field,value));self.assertFalse(report.analyse(out)['passed'])
    def test_handles_and_pending_DMA_rejected(self):
        for op,value in (('WaitVsync','42'),('CloseDMAEvent','42'),('IsFinishedReceiving','0')):
            out=self.fixture();self.edit(out,'calls.csv',lambda rows:next(r for r in rows if r['phase']=='5' and r['operation']==op).__setitem__('value',value));self.assertFalse(report.analyse(out)['passed'])
    def test_missing_ROI_or_journal_rejected(self):
        for kind in ('raw','journal'):
            out=self.fixture();p=next(out.glob('*.yuyv')) if kind=='raw' else out/'startup_journal.txt';p.unlink();self.receipts(out)
            if kind=='journal':
                with self.assertRaises(ValueError):report.analyse(out)
            else:self.assertFalse(report.analyse(out)['passed'])
    def test_unknown_source_and_integrity_fail_closed(self):
        out=self.fixture();p=out/'manifest.txt';p.write_text(p.read_text().replace(CCM_SOURCE,'unknown'));self.receipts(out)
        with self.assertRaisesRegex(ValueError,'unregistered'):report.analyse(out)
        with (out/'reads.csv').open('a') as f:f.write('tamper')
        with self.assertRaisesRegex(ValueError,'integrity'):report.analyse(out)

if __name__=='__main__':unittest.main()
