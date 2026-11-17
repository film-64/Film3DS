"""Real C producer evidence with explicit simulated update models and faults."""
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
import exposure_isp_update_report as report


class UpdateTests(unittest.TestCase):
    def fixture(self,scenario):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        out=Path(temporary.name)
        source=ROOT/f'build/worktrees/camera-deep-probe/build/isp-update-{scenario}'
        self.assertTrue(source.is_dir(),'run ISP_UPDATE_ONLY=1 ./build/probe-driver first')
        for path in source.iterdir():shutil.copyfile(path,out/path.name)
        with (out/'manifest.txt').open('a') as f:f.write(f'END complete={int(scenario<3)} aborted=0 io_error=0\n')
        self.receipts(out);return out
    def receipts(self,out):
        entries=[]
        for path in sorted(out.iterdir()):
            if path.name=='received-ram-sha256.json':continue
            b=path.read_bytes();entries.append(dict(name=path.name,bytes=len(b),crc32=f'{zlib.crc32(b):08x}',sha256=hashlib.sha256(b).hexdigest()))
        (out/'received-ram-sha256.json').write_text(json.dumps(entries))
    def test_repeated_updates_are_valid_observations_not_unity_calibration(self):
        result=report.analyse(self.fixture(0))
        self.assertTrue(result['passed'],result['errors'])
        self.assertFalse(result['calibration_validated'])
        self.assertEqual([a['outcome'] for a in result['arms']],['rewrite_overwritten']*4)
        self.assertTrue(all(a['observed']['fresh_frame']==[133]*4 for a in result['arms']))
    def test_once_only_update_is_distinguished_from_per_frame_update(self):
        result=report.analyse(self.fixture(1))
        self.assertTrue(result['passed'],result['errors'])
        self.assertTrue(all(a['observed']['first_frame']==[133]*4 and a['outcome']=='rewrite_held' for a in result['arms']))
    def test_different_initial_values_are_preserved_when_no_update_occurs(self):
        result=report.analyse(self.fixture(2));self.assertTrue(result['passed'],result['errors'])
        self.assertEqual([a['observed']['first_frame'] for a in result['arms']],[[96]*4,[128]*4,[160]*4,[128]*4])
    def test_DMA_timeout_is_inconclusive_not_a_held_gain(self):
        result=report.analyse(self.fixture(3));self.assertFalse(result['passed'])
        self.assertEqual(result['arms'][-1]['outcome'],'inconclusive')
    def test_control_refresh_restore_mode_and_unit_gain_failures_reject(self):
        for scenario in range(4,9):self.assertFalse(report.analyse(self.fixture(scenario))['passed'],scenario)
    def test_extra_continuous_start_and_hidden_rewrite_reject(self):
        for hidden in (False,True):
            out=self.fixture(0)
            with (out/'calls.csv').open() as f:reader=csv.DictReader(f);fields=reader.fieldnames;rows=list(reader)
            copied=next(dict(r) for r in rows if r['phase']=='8' and r['operation']==('WriteRegisterI2c' if hidden else 'StartCapture'))
            if hidden:copied.update(label='hidden_repair',addr='32d4',value='128')
            index=next(i for i,r in enumerate(rows) if r['phase']=='8' and r['label']=='isp_update_continuous_128_f1' and r['operation']=='WaitFrame')
            rows.insert(index,copied)
            with (out/'calls.csv').open('w') as f:writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(rows)
            self.receipts(out);self.assertFalse(report.analyse(out)['passed'])
    def test_lifecycle_cleanup_after_arm_is_not_an_extra_stream_restart(self):
        out=self.fixture(0)
        with (out/'calls.csv').open() as f:reader=csv.DictReader(f);fields=reader.fieldnames;rows=list(reader)
        stopped=next(dict(r) for r in rows if r['phase']=='8' and r['operation']=='StopCapture')
        stopped['label']='cleanup';rows.append(stopped)
        with (out/'calls.csv').open('w') as f:writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(rows)
        self.receipts(out);self.assertTrue(report.analyse(out)['passed'])
    def test_wrong_identity_failed_read_unknown_source_and_integrity_reject(self):
        out=self.fixture(0)
        with (out/'reads.csv').open() as f:reader=csv.DictReader(f);fields=reader.fieldnames;rows=list(reader)
        for row in rows:
            if row['name']=='isp_update_state':row['sensor']='2';break
        with (out/'reads.csv').open('w') as f:writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(rows)
        self.receipts(out);self.assertFalse(report.analyse(out)['passed'])
        manifest=(out/'manifest.txt').read_text();(out/'manifest.txt').write_text(manifest.replace(report.UPDATE_SOURCE,'unregistered'))
        self.receipts(out)
        with self.assertRaisesRegex(ValueError,'unregistered'):report.analyse(out)
        with (out/'reads.csv').open('a') as f:f.write('tamper')
        with self.assertRaisesRegex(ValueError,'integrity failure'):report.analyse(out)


if __name__=='__main__':unittest.main()
