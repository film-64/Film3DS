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
import camera_ccm_boot_report as report
from exposure_sweep_contract import CCM_BOOT_SOURCE as CCM_SOURCE

class CCMBootTests(unittest.TestCase):
    def receipts(self,out):
        entries=[]
        for p in sorted(out.iterdir()):
            if p.name=='received-ram-sha256.json':continue
            b=p.read_bytes();entries.append(dict(name=p.name,bytes=len(b),crc32=f'{zlib.crc32(b):08x}',sha256=hashlib.sha256(b).hexdigest()))
        (out/'received-ram-sha256.json').write_text(json.dumps(entries))
    def fixture(self,n=0):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup);out=Path(tmp.name)
        source=ROOT/f'build/worktrees/camera-deep-probe/build/ccm-boot-{n}'
        self.assertTrue(source.is_dir(),'run CCM_BOOT_ONLY=1 ./build/probe-driver first')
        for p in source.iterdir():shutil.copyfile(p,out/p.name)
        with (out/'manifest.txt').open('a') as f:f.write(f'END complete={int(n<3 or n==10)} aborted=0 io_error=0\n')
        self.receipts(out);return out
    def edit(self,out,file,fn):
        p=out/file
        with p.open() as f:r=csv.DictReader(f);fields=r.fieldnames;rows=list(r)
        fn(rows)
        with p.open('w') as f:w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
        self.receipts(out)
    def test_three_actual_producer_models(self):
        for n,conclusion in enumerate(('CCM_bit6_changes_reload_value_to_unity','CCM_bit6_not_sufficient_to_stop_update','no_overwrite_observed')):
            r=report.analyse(self.fixture(n));self.assertTrue(r['passed'],r['errors']);self.assertEqual(r['conclusion'],conclusion)
            self.assertFalse(r['calibration_validated']);self.assertFalse(r['writer_identified']);self.assertFalse(r['timing_cause_identified'])
    def test_reload_value_change_and_suppressed_update_are_distinct(self):
        r=report.analyse(self.fixture(10));self.assertTrue(r['passed'],r['errors'])
        self.assertEqual(r['conclusion'],'CCM_bit6_suppresses_observed_update')
        self.assertEqual([t['write_target'] for t in r['arms'][1]['trials']],[96,128,160,128])
    def test_producer_failures_cannot_pass(self):
        for n in list(range(3,10))+list(range(11,15)):
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
    def test_successful_bytes_recover_FFFF_without_accepting_failed_read(self):
        r=report.analyse(self.fixture());self.assertTrue(r['passed'],r['errors'])
        self.assertEqual(r['arms'][1]['snapshots']['before_stream']['233e'],65535)
        for field,value in (('value','256'),('result','c9602fef'),('addr','a999')):
            out=self.fixture()
            self.edit(out,'reads.csv',lambda rows:next(r for r in rows if r['name']=='ccm_byte').__setitem__(field,value))
            self.assertFalse(report.analyse(out)['passed'])
    def test_native_reference_and_derived_value_cannot_be_forged(self):
        for kind in ('native','word','alias'):
            out=self.fixture()
            if kind=='native':self.edit(out,'reads.csv',lambda rows:next(r for r in rows if r['name']=='ccm_native_reference').__setitem__('value','411'))
            else:
                p=out/'manifest.txt';s=p.read_text()
                if kind=='word':s=s.replace('addr=233e msb=255 lsb=255 value=65535','addr=233e msb=255 lsb=255 value=0',1)
                else:s=s.replace('combined=410 verified=1','combined=411 verified=1',1)
                p.write_text(s);self.receipts(out)
            self.assertFalse(report.analyse(out)['passed'],kind)
    def test_missing_journal_fault_and_broken_sequence_rejected(self):
        for kind in ('missing','fault','sequence','apt'):
            out=self.fixture();p=out/'startup_journal.txt'
            if kind=='missing':p.unlink()
            elif kind=='apt':p.write_text(p.read_text().replace('stage=16','stage=25',1))
            elif kind=='fault':p.write_text(p.read_text().replace('stage=16','stage=255',1))
            else:p.write_text(p.read_text().replace('seq=0','seq=99',1))
            self.receipts(out);self.assertFalse(report.analyse(out)['passed'])
    def test_startup_read_boundaries_cannot_be_forged(self):
        out=self.fixture();p=out/'startup_journal.txt';p.write_text(p.read_text().replace('stage=16 addr=0000a332','stage=16 addr=0000a999',1));self.receipts(out)
        self.assertFalse(report.analyse(out)['passed'])
    def test_CCM_mode_proof_and_before_Refresh_order_required(self):
        for action in ('missing','wrong_value','after_refresh'):
            out=self.fixture()
            def fn(rows):
                wr=next(r for r in rows if r['phase']=='6' and r['label']=='ccm_mode' and r['operation']=='WriteMcuVariableI2c')
                if action=='missing':rows.remove(wr)
                elif action=='wrong_value':wr['value']='33'
                else:
                    rows.remove(wr);i=next(i for i,r in enumerate(rows) if r['phase']=='6' and r['addr']=='a103' and r['operation']=='WriteMcuVariableI2c');rows.insert(i+1,wr)
            self.edit(out,'calls.csv',fn);self.assertFalse(report.analyse(out)['passed'])
    def test_source_cannot_masquerade_as_HG(self):
        from camera_hg_report import analyse
        with self.assertRaisesRegex(ValueError,'unregistered'):analyse(self.fixture())
    def test_unknown_source_and_integrity_fail_closed(self):
        out=self.fixture();p=out/'manifest.txt';p.write_text(p.read_text().replace(CCM_SOURCE,'unknown'));self.receipts(out)
        with self.assertRaisesRegex(ValueError,'unregistered'):report.analyse(out)
        with (out/'reads.csv').open('a') as f:f.write('tamper')
        with self.assertRaisesRegex(ValueError,'integrity'):report.analyse(out)

if __name__=='__main__':unittest.main()
