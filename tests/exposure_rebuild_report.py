"""Contract tests over the real native C producer, including proof corruption."""
import csv,hashlib,json,shutil,sys,tempfile,unittest,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import exposure_rebuild_report as report
from exposure_sweep_contract import EXPOSURE_REBUILD_SOURCE

class RebuildTests(unittest.TestCase):
    def receipts(self,p):
        rows=[]
        for f in sorted(p.iterdir()):
            if f.name=='received-ram-sha256.json':continue
            b=f.read_bytes();rows.append(dict(name=f.name,bytes=len(b),crc32=f'{zlib.crc32(b):08x}',sha256=hashlib.sha256(b).hexdigest()))
        (p/'received-ram-sha256.json').write_text(json.dumps(rows))
    def fixture(self,n=0):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup);p=Path(tmp.name);source=ROOT/f'build/worktrees/camera-deep-probe/build/exposure-rebuild-v2-{n}'
        self.assertTrue(source.is_dir(),'run EXPOSURE_REBUILD_ONLY=1 ./build/probe-driver first')
        for f in source.iterdir():shutil.copyfile(f,p/f.name)
        with (p/'manifest.txt').open('a') as f:f.write(f'END complete={int(n in (0,1,9,10,11,13,14,15))} aborted=0 io_error=0\n')
        self.receipts(p);return p
    def edit(self,p,name,fn):
        path=p/name
        with path.open() as f:r=csv.DictReader(f);fields=r.fieldnames;rows=list(r)
        fn(rows)
        with path.open('w') as f:w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
        self.receipts(p)
    def test_actual_successful_producer(self):
        r=report.analyse(self.fixture());self.assertTrue(r['passed'],r['errors']);self.assertEqual(r['points'],74);self.assertEqual(r['DMA_frames'],748);self.assertEqual(r['raw_target_images'],296)
        self.assertTrue(all(b['timing_estimate_usable'] for b in r['blocks']))
        self.assertFalse(r['calibration_validated']);self.assertFalse(r['optical_time_verified']);self.assertFalse(r['lighting_assumed_steady'])
    def test_no_FD_effect_and_pipeline_delay_are_data(self):
        for n in (1,9,13):
            r=report.analyse(self.fixture(n));self.assertTrue(r['passed'],r['errors'])
            if n==13:
                for b in r['blocks']:
                    on=next(x for x in b['points'] if x['kind']=='FD_on');self.assertEqual(on['observed_coarse'],[8]);self.assertIsNone(on['estimated_seconds'])
    def test_FD_tail_and_persistent_update_are_distinct_data(self):
        for n,held in ((14,True),(15,False)):
            r=report.analyse(self.fixture(n));self.assertTrue(r['passed'],r['errors'])
            for b in r['blocks']:self.assertEqual(next(x for x in b['points'] if x['kind']=='FD_off_after')['FD_after_reassert_held'],held)
    def test_dark_and_clipped_keep_evidence_without_fake_coverage(self):
        for n in (10,11):
            r=report.analyse(self.fixture(n));self.assertTrue(r['passed'],r['errors']);self.assertTrue(all(not b['anchor_coverage'] and b['primary_anchor']==5 for b in r['blocks']))
    def test_actual_transport_control_restore_and_allocation_failures(self):
        for n in list(range(2,9))+[12]:
            r=report.analyse(self.fixture(n));self.assertFalse(r['passed'],n)
    def test_extra_receiving_and_wrong_buffer_rejected(self):
        for kind in ('extra','slot','event'):
            p=self.fixture()
            if kind=='extra':self.edit(p,'calls.csv',lambda rows:rows.append(dict(next(r for r in rows if r['label']=='rb_dma_0010' and r['operation']=='SetReceiving'))))
            elif kind=='slot':self.edit(p,'rebuild_frames.csv',lambda rows:rows[10].__setitem__('slot','9'))
            else:self.edit(p,'rebuild_frames.csv',lambda rows:rows[10].__setitem__('event','42'))
            self.assertFalse(report.analyse(p)['passed'],kind)
    def test_unregistered_work_and_color_conversion_rejected(self):
        p=self.fixture();self.edit(p,'calls.csv',lambda rows:rows.insert(next(i for i,r in enumerate(rows) if r['label']=='rb_dma_0010'),dict(next(r for r in rows if r['label']=='rb_dma_0010'),operation='SwitchContext')));self.assertFalse(report.analyse(p)['passed'])
        p=self.fixture();f=p/'manifest.txt';f.write_text(f.read_text().replace('REBUILD_COLOR_MATRIX value=1 verified=1','REBUILD_COLOR_MATRIX value=9 verified=1'));self.receipts(p);self.assertFalse(report.analyse(p)['passed'])
    def test_fake_fixed_control_proof_rejected(self):
        for a in ('3012','a102','32d4','3014'):
            p=self.fixture();self.edit(p,'reads.csv',lambda rows:next(r for r in rows if r['name']=='rebuild_state' and r['when']=='after' and r['addr']==a).__setitem__('value','999'));self.assertFalse(report.analyse(p)['passed'],a)
    def test_misplaced_adaptive_point_or_forged_score_rejected(self):
        for field,value in (('anchor','99'),('score_y','128.123')):
            p=self.fixture();self.edit(p,'rebuild_points.csv',lambda rows:rows[2].__setitem__(field,value));self.assertFalse(report.analyse(p)['passed'])
    def test_image_statistics_and_bands_recomputed(self):
        for file,field,value in (('rebuild_frames.csv','y_mean','99.5'),('rebuild_bands.csv','b00','99.5')):
            p=self.fixture();self.edit(p,file,lambda rows:rows[0].__setitem__(field,value));self.assertFalse(report.analyse(p)['passed'])
    def test_missing_or_extra_image_rejected(self):
        for kind in ('missing','extra'):
            p=self.fixture()
            if kind=='missing':next(p.glob('*rb_p000_f000.yuyv')).unlink()
            else:(p/'s1-p999-rb_p999_f000.yuyv').write_bytes(bytes(24576))
            self.receipts(p);self.assertFalse(report.analyse(p)['passed'])
    def test_alias_error_or_forged_live_coherence_rejected(self):
        p=self.fixture();self.edit(p,'reads.csv',lambda rows:next(r for r in rows if r['name']=='matrix_alias_hi').__setitem__('value','9'));self.assertFalse(report.analyse(p)['passed'])
        p=self.fixture();f=p/'manifest.txt';f.write_text(f.read_text().replace('match_before=1 match_after=1','match_before=0 match_after=0',1));self.receipts(p);self.assertFalse(report.analyse(p)['passed'])
    def test_missing_or_fault_journal_rejected(self):
        p=self.fixture();f=p/'startup_journal.txt';f.write_text(f.read_text().replace('stage=16','stage=255',1));self.receipts(p);self.assertFalse(report.analyse(p)['passed'])
        p=self.fixture();(p/'startup_journal.txt').unlink();self.receipts(p)
        with self.assertRaisesRegex(ValueError,'missing'):report.analyse(p)
    def test_missing_or_retimed_reassert_rejected(self):
        p=self.fixture();f=p/'manifest.txt';f.write_text(f.read_text().replace('frame=8 begin_ms=','frame=7 begin_ms=',1));self.receipts(p);self.assertFalse(report.analyse(p)['passed'])
        p=self.fixture();self.edit(p,'calls.csv',lambda rows:rows.remove(next(r for r in rows if r['label'].startswith('rb_reassert_'))));self.assertFalse(report.analyse(p)['passed'])
        p=self.fixture();self.edit(p,'calls.csv',lambda rows:next(r for r in rows if r['label'].startswith('rb_reassert_')).__setitem__('phase','4'));self.assertFalse(report.analyse(p)['passed'])
    def test_source_and_integrity_fail_closed(self):
        p=self.fixture();f=p/'manifest.txt';f.write_text(f.read_text().replace(EXPOSURE_REBUILD_SOURCE,'unknown'));self.receipts(p)
        with self.assertRaisesRegex(ValueError,'unregistered'):report.analyse(p)
        with (p/'reads.csv').open('a') as f:f.write('tamper')
        with self.assertRaisesRegex(ValueError,'integrity'):report.analyse(p)

if __name__=='__main__':unittest.main()
