"""Real C evidence: only new, cleared, timestamped VSYNC proves frame cycles."""
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
import exposure_no_receive_report as report


class NoReceiveTests(unittest.TestCase):
    def receipts(self,out):
        entries=[]
        for path in sorted(out.iterdir()):
            if path.name=='received-ram-sha256.json':continue
            b=path.read_bytes();entries.append(dict(name=path.name,bytes=len(b),crc32=f'{zlib.crc32(b):08x}',sha256=hashlib.sha256(b).hexdigest()))
        (out/'received-ram-sha256.json').write_text(json.dumps(entries))
    def fixture(self,scenario=0):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup);out=Path(temporary.name)
        source=ROOT/f'build/worktrees/camera-deep-probe/build/no-receive-{scenario}'
        self.assertTrue(source.is_dir(),'run NO_RECEIVE_ONLY=1 ./build/probe-driver first')
        for path in source.iterdir():shutil.copyfile(path,out/path.name)
        with (out/'manifest.txt').open('a') as f:f.write(f'END complete={int(scenario<3)} aborted=0 io_error=0\n')
        self.receipts(out);return out
    def edit_csv(self,out,name,edit):
        with (out/name).open() as f:reader=csv.DictReader(f);fields=reader.fieldnames;rows=list(reader)
        edit(rows)
        with (out/name).open('w') as f:writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(rows)
        self.receipts(out)
    def test_new_receive_not_necessary_when_both_models_overwrite(self):
        r=report.analyse(self.fixture());self.assertTrue(r['passed'],r['errors'])
        self.assertEqual(r['conclusion'],'new_SetReceiving_not_necessary');self.assertFalse(r['writer_identified'])
        self.assertFalse(r['calibration_validated']);self.assertEqual(r['arms'][1]['raw_provenance'],'first_DMA_only')
        self.assertEqual(r['arms'][1]['observed']['3:post_vsync'],[133]*4)
    def test_DMA_related_and_no_overwrite_models_remain_distinct(self):
        for n,expected in ((1,'Receive_related_path_not_excluded'),(2,'overwrite_not_reproduced')):
            r=report.analyse(self.fixture(n));self.assertTrue(r['passed'],r['errors']);self.assertEqual(r['conclusion'],expected)
    def test_timeout_stale_event_timing_clear_control_restore_failures_are_inconclusive(self):
        for n in range(3,10):
            r=report.analyse(self.fixture(n));self.assertFalse(r['passed'],n);self.assertEqual(r['conclusion'],'inconclusive')
    def test_hidden_receive_refresh_or_write_during_window_rejected(self):
        for op in ('SetReceiving','StartCapture','StopCapture','SwitchContext','ClearBuffer','WriteMcuVariableI2c','WriteRegisterI2c'):
            out=self.fixture()
            def edit(rows):
                i=next(i for i,r in enumerate(rows) if r['phase']=='6' and r['operation']=='WaitVsync')
                row=dict(rows[i],operation=op,label='hidden',addr='a103' if op=='WriteMcuVariableI2c' else '32d4')
                rows.insert(i,row)
            self.edit_csv(out,'calls.csv',edit);self.assertFalse(report.analyse(out)['passed'],op)
    def test_fake_pending_DMA_proof_and_wrong_handle_rejected(self):
        for op,field,value in (('IsFinishedReceiving','value','0'),('CloseDMAEvent','value','42'),('WaitVsync','value','42'),('GetLatestVsyncTiming','value','2')):
            out=self.fixture()
            def edit(rows):next(r for r in rows if r['phase']=='6' and r['operation']==op)[field]=value
            self.edit_csv(out,'calls.csv',edit);self.assertFalse(report.analyse(out)['passed'],op)
    def test_forged_or_missing_VSYNC_time_proof_rejected(self):
        for old,new in (('latest=1200','latest=1000'),('when=latest raw=1200','when=latest raw=1000'),('NO_RECEIVE_TIMING observation=1 when=latest','MISSING observation=1 when=latest')):
            out=self.fixture();p=out/'manifest.txt';p.write_text(p.read_text().replace(old,new));self.receipts(out)
            r=report.analyse(out);self.assertFalse(r['passed']);self.assertEqual(r['conclusion'],'inconclusive')
    def test_failed_wrong_sensor_or_pre_VSYNC_read_rejected(self):
        for field,value in (('result','d900182f'),('sensor','2'),('time_ms','0')):
            out=self.fixture()
            def edit(rows):next(r for r in rows if r['name']=='no_receive_red')[field]=value
            self.edit_csv(out,'reads.csv',edit);self.assertFalse(report.analyse(out)['passed'])
    def test_later_lifecycle_cleanup_is_allowed(self):
        out=self.fixture()
        def edit(rows):rows.append(dict(next(r for r in rows if r['phase']=='6' and r['operation']=='StopCapture'),label='cleanup'))
        self.edit_csv(out,'calls.csv',edit);self.assertTrue(report.analyse(out)['passed'])
    def test_unknown_source_and_integrity_fail_closed(self):
        out=self.fixture();p=out/'manifest.txt';p.write_text(p.read_text().replace(report.NO_RECEIVE_SOURCE,'unknown'))
        self.receipts(out)
        with self.assertRaisesRegex(ValueError,'unregistered'):report.analyse(out)
        with (out/'reads.csv').open('a') as f:f.write('tamper')
        with self.assertRaisesRegex(ValueError,'integrity failure'):report.analyse(out)


if __name__=='__main__':unittest.main()
