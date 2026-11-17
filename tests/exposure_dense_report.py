"""Contract tests over the real native C producer, including proof corruption."""
import csv,hashlib,json,shutil,sys,tempfile,unittest,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import exposure_dense_report as report
from exposure_sweep_contract import EXPOSURE_DENSE_SOURCE

class DenseTests(unittest.TestCase):
    def receipts(self,p):
        rows=[]
        for f in sorted(p.iterdir()):
            if f.name=='received-ram-sha256.json':continue
            b=f.read_bytes();rows.append(dict(name=f.name,bytes=len(b),crc32=f'{zlib.crc32(b):08x}',sha256=hashlib.sha256(b).hexdigest()))
        (p/'received-ram-sha256.json').write_text(json.dumps(rows))
    def fixture(self,n=0):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup);p=Path(tmp.name);source=ROOT/f'build/worktrees/camera-deep-probe/build/exposure-dense-{n}'
        self.assertTrue(source.is_dir(),'run EXPOSURE_REBUILD_ONLY=1 ./build/probe-driver first')
        for f in source.iterdir():shutil.copyfile(f,p/f.name)
        with (p/'manifest.txt').open('a') as f:f.write(f'END complete={int(n<4)} aborted=0 io_error=0\n')
        self.receipts(p);return p
    def edit(self,p,name,fn):
        path=p/name
        with path.open() as f:r=csv.DictReader(f);fields=r.fieldnames;rows=list(r)
        fn(rows)
        with path.open('w') as f:w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
        self.receipts(p)
    def test_real_dense_producer(self):
        r=report.analyse(self.fixture());self.assertTrue(r['passed'],r['errors']);self.assertEqual(r['points'],74);self.assertEqual(r['DMA_frames'],444);self.assertEqual(r['raw_target_images'],296)
        self.assertEqual([len([p for p in b['points'] if p['kind']=='coarse']) for b in r['blocks']],[19,15]);self.assertFalse(r['calibration_validated'])
    def test_latency_dark_and_clip_are_data(self):
        for n in (1,2,3):
            r=report.analyse(self.fixture(n));self.assertTrue(r['passed'],r['errors'])
            if n in (2,3):self.assertTrue(all(not b['anchor_coverage'] for b in r['blocks']))
    def test_real_producer_failures(self):
        for n in range(4,12):self.assertFalse(report.analyse(self.fixture(n))['passed'],n)
    def test_wrong_or_fake_control_proof(self):
        for a in ('a102','3012','32d4'):
            p=self.fixture();self.edit(p,'reads.csv',lambda rows:next(r for r in rows if r['name']=='rebuild_state' and r['when']=='after' and r['addr']==a).__setitem__('value','999'));self.assertFalse(report.analyse(p)['passed'])
    def test_unknown_work_and_buffer_slot(self):
        p=self.fixture();self.edit(p,'calls.csv',lambda rows:rows.append(dict(next(r for r in rows if r['label']=='rb_dma_0010'),operation='SwitchContext')));self.assertFalse(report.analyse(p)['passed'])
        p=self.fixture();self.edit(p,'rebuild_frames.csv',lambda rows:rows[10].__setitem__('slot','9'));self.assertFalse(report.analyse(p)['passed'])
    def test_adaptive_score_and_point_plan(self):
        for field,value in (('anchor','99'),('score_y','128.123')):
            p=self.fixture();self.edit(p,'rebuild_points.csv',lambda rows:rows[2].__setitem__(field,value));self.assertFalse(report.analyse(p)['passed'])
    def test_raw_statistics_and_bands(self):
        for file,field,value in (('rebuild_frames.csv','y_mean','99.5'),('rebuild_bands.csv','b00','99.5')):
            p=self.fixture();self.edit(p,file,lambda rows:rows[0].__setitem__(field,value));self.assertFalse(report.analyse(p)['passed'])
    def test_alias_proof_or_journal(self):
        p=self.fixture();self.edit(p,'reads.csv',lambda rows:next(r for r in rows if r['name']=='matrix_alias_hi').__setitem__('value','9'));self.assertFalse(report.analyse(p)['passed'])
        p=self.fixture();f=p/'startup_journal.txt';f.write_text(f.read_text().replace('stage=16','stage=255',1));self.receipts(p);self.assertFalse(report.analyse(p)['passed'])
    def test_missing_image(self):
        p=self.fixture();next(p.glob('*rb_p000_f000.yuyv')).unlink();self.receipts(p);self.assertFalse(report.analyse(p)['passed'])
    def test_unknown_source_integrity_and_old_scope(self):
        p=self.fixture();f=p/'manifest.txt';f.write_text(f.read_text().replace(EXPOSURE_DENSE_SOURCE,'unknown'));self.receipts(p)
        with self.assertRaisesRegex(ValueError,'unregistered'):report.analyse(p)
        with (p/'reads.csv').open('a') as f:f.write('tamper')
        with self.assertRaisesRegex(ValueError,'integrity'):report.analyse(p)
        from exposure_rebuild_report import analyse
        with self.assertRaisesRegex(ValueError,'unregistered'):analyse(self.fixture())

if __name__=='__main__':unittest.main()
