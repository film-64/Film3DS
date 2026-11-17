"""Diagnostic proof checks using explicitly synthetic MCU/IFP rows."""
import copy
import csv
import hashlib,json,tempfile,zlib
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import exposure_isp_report as report

class ProofTests(unittest.TestCase):
    def fixture(self):
        values=dict(zip(report.MCU,[10,0,3,33,168,162,133,128]))
        values.update(dict.fromkeys(report.IFP,128))
        return [dict(sensor='1',label='sweep_s13_c0004',phase='5',frame=str(frame),when=when,space='mcu' if addr in report.MCU else 'reg',
                     addr=addr,value=str(value),result='00000000')
                for frame in range(8,12) for when in ('before','after') for addr,value in values.items()]
    def test_cached_awb_values_are_not_confused_with_forced_physical_gains(self):
        frames,errors=report.measured(self.fixture())
        self.assertFalse(errors);self.assertEqual(frames[0]['values']['a34e'],168)
        self.assertEqual(frames[0]['values']['32d4'],128)
    def test_missing_duplicate_and_failed_reads_reject(self):
        original=self.fixture()
        for rows in (original[1:],original+[original[0]],copy.deepcopy(original)):
            if len(rows)==len(original):rows[0]['result']='c960454b'
            self.assertTrue(report.measured(rows)[1])
    def test_live_driver_force_bit_state_and_physical_drift_reject(self):
        for addr,value in [('a102',15),('a355',1),('a104',1),('32d4',168)]:
            rows=self.fixture()
            next(r for r in rows if r['addr']==addr)['value']=str(value)
            self.assertTrue(report.measured(rows)[1],addr)
    def test_preview_capture_transition_during_measured_frames_rejects(self):
        rows=self.fixture();next(r for r in rows if r['addr']=='a104')['value']='7'
        self.assertIn('sequencer_state_changed_during_measurement',report.measured(rows)[1])
    def test_missing_frame_rejects_without_inventing_state_change(self):
        rows=[r for r in self.fixture() if not (r['frame']=='8' and r['when']=='before')]
        errors=report.measured(rows)[1]
        self.assertTrue(errors)
        self.assertNotIn('sequencer_state_changed_during_measurement',errors)
    def test_unknown_source_and_wrong_identity_reject(self):
        with self.assertRaises(ValueError):report.measured(self.fixture(),'unregistered')
        for key,value in [('sensor','2'),('label','another_test')]:
            rows=self.fixture();rows[0][key]=value
            self.assertTrue(report.measured(rows)[1])
    def test_pending_command_and_state_33_reject(self):
        for addr,value in [('a103',5),('a104',33)]:
            rows=self.fixture();next(r for r in rows if r['addr']==addr)['value']=str(value)
            self.assertTrue(report.measured(rows)[1])
    def test_refresh_uninterpreted_low_bits_do_not_replace_control_proof(self):
        rows=self.fixture()
        for r in rows:
            if r['addr']=='a102':r['value']='14'
            if r['addr']=='a355':r['value']='42' if r['when']=='before' else '58'
        self.assertFalse(report.measured(rows,report.REFRESH_SOURCE)[1])
        next(r for r in rows if r['addr']=='a355')['value']='26'
        self.assertTrue(report.measured(rows,report.REFRESH_SOURCE)[1])
    def test_post_refresh_manual_requires_coarse_4_and_drivers_off(self):
        rows=self.fixture()
        for frame in range(8,12):
            for when in ('before','after'):
                rows.append(dict(sensor='1',label='sweep_s13_c0004',phase='5',frame=str(frame),when=when,
                                 space='reg',addr='3012',value='4',result='00000000'))
        self.assertFalse(report.measured(rows,report.MANUAL_SOURCE)[1])
        next(r for r in rows if r['addr']=='3012')['value']='264'
        self.assertTrue(report.measured(rows,report.MANUAL_SOURCE)[1])
        next(r for r in rows if r['addr']=='3012')['value']='4'
        next(r for r in rows if r['addr']=='a102')['value']='14'
        self.assertTrue(report.measured(rows,report.MANUAL_SOURCE)[1])
    def test_refresh_has_explicit_AWB_on_proof_and_status_bits_can_change(self):
        rows=self.fixture()
        for r in rows:
            if r['addr']=='a102':r['value']='14'
            if r['addr']=='a355':r['value']='34' if r['when']=='before' else '33'
        self.assertFalse(report.measured(rows,report.REFRESH_SOURCE)[1])
        self.assertTrue(report.measured(rows)[1])
        for r in rows:
            if r['addr']=='a102':r['value']='10'
        self.assertTrue(report.measured(rows,report.REFRESH_SOURCE)[1])
    def test_real_C_producer_report_integrity_and_refresh_proof(self):
        root=Path(__file__).resolve().parents[1]
        fixture=root/'build/worktrees/camera-deep-probe/build/exposure-sweep-v2-12'
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            for name in ('manifest.txt','reads.csv','frames.csv','calls.csv'):
                (p/name).write_bytes((fixture/name).read_bytes())
            with (p/'manifest.txt').open('a') as f:f.write('END complete=1 aborted=0 io_error=0 failures=0\n')
            def receipts():
                entries=[]
                for name in ('manifest.txt','reads.csv','frames.csv','calls.csv'):
                    b=(p/name).read_bytes()
                    entries.append(dict(name=name,bytes=len(b),sha256=hashlib.sha256(b).hexdigest(),crc32=f'{zlib.crc32(b):08x}'))
                (p/'received-ram-sha256.json').write_text(json.dumps(entries))
            receipts();result=report.analyse(p)
            self.assertTrue(result['passed'],result['errors'])
            self.assertFalse(result['calibration_validated'])
            manifest=(p/'manifest.txt').read_text()
            (p/'manifest.txt').write_text('\n'.join(line for line in manifest.splitlines() if not line.startswith('SEQ_REFRESH ')))
            receipts();self.assertIn('refresh_not_verified',report.analyse(p)['errors'])
            with (p/'reads.csv').open('a') as f:f.write('tampered')
            with self.assertRaisesRegex(ValueError,'integrity failure'):report.analyse(p)
    def test_manual_C_write_order_and_no_refresh_during_capture(self):
        root=Path(__file__).resolve().parents[1]
        fixture=root/'build/worktrees/camera-deep-probe/build/exposure-sweep-v2-21'
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            for name in ('manifest.txt','reads.csv','frames.csv','calls.csv'):
                (p/name).write_bytes((fixture/name).read_bytes())
            with (p/'manifest.txt').open('a') as f:f.write('END complete=1 aborted=0 io_error=0 failures=0\n')
            def receipts():
                entries=[]
                for name in ('manifest.txt','reads.csv','frames.csv','calls.csv'):
                    b=(p/name).read_bytes();entries.append(dict(name=name,bytes=len(b),sha256=hashlib.sha256(b).hexdigest(),crc32=f'{zlib.crc32(b):08x}'))
                (p/'received-ram-sha256.json').write_text(json.dumps(entries))
            receipts();result=report.analyse(p)
            self.assertTrue(result['passed'],result['errors'])
            with (p/'calls.csv').open() as f:
                reader=csv.DictReader(f);fields=reader.fieldnames;calls=list(reader)
            index=next(i for i,r in enumerate(calls) if r['label']=='sweep_refresh_restore' and r['operation']=='WriteMcuVariableI2c')
            restore=calls.pop(index)
            index=next(i for i,r in enumerate(calls) if r['label']=='sweep_s13_c0004' and r['operation']=='StartCapture')
            calls.insert(index+1,restore)
            with (p/'calls.csv').open('w') as f:
                writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(calls)
            receipts();self.assertIn('refresh_during_manual_measurement',report.analyse(p)['errors'])
if __name__=='__main__':unittest.main()
