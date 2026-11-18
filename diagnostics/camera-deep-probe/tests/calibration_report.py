"""Verify actual C-driver fixture evidence; fake completion/lock strings cannot replace frames."""
import csv,importlib.util,sys,tempfile,shutil,json
from pathlib import Path
root=Path(__file__).parents[1];sys.path.insert(0,str(root/'tools'))
import calibration_report as report

with tempfile.TemporaryDirectory() as temporary:
    out=Path(temporary)
    for target,source in [('calls.csv','build/probe-calls.tmp'),('reads.csv','build/probe-reads.tmp'),('frames.csv','build/probe-frames.tmp')]:
        shutil.copyfile(root/source,out/target)
    for name in ('calibration_plan.csv','frame_burst.csv','roi.csv','noise.csv'):shutil.copyfile(root/'build/calibration-test'/name,out/name)
    (out/'manifest.txt').write_text('REQUEST only_sensor=1 first_phase=3 last_phase=60\nCALIBRATION contract=1 expected_stages=58 scope=full\nSENSOR_END sensor=1 complete=1\nMEASUREMENTS expected=58 complete=58 all_complete=1\nEND complete=1 aborted=0 io_error=0\n')
    result=report.summarize(out)
    assert result['measurements_complete'] and result['verified_measured_frames']==678
    assert result['absolute_iso'] is None and result['absolute_shutter_seconds'] is None
    original=(out/'frames.csv').read_text();lines=original.splitlines(True)
    (out/'frames.csv').write_text(''.join(lines[:-1]));assert not report.summarize(out)['measurements_complete']
    (out/'frames.csv').write_text(original)
    original=(out/'reads.csv').read_text()
    rows=list(csv.DictReader(original.splitlines()));fields=list(rows[0])
    victim=next(r for r in rows if r['phase']=='4' and r['frame']=='8' and r['addr']=='a11d' and r['when']=='after')
    victim['value']='1'
    with (out/'reads.csv').open('w',newline='') as file:
        writer=csv.DictWriter(file,fieldnames=fields);writer.writeheader();writer.writerows(rows)
    result=report.summarize(out);assert not result['measurements_complete'] and not result['phases']['4']['locked_controls_held']
    (out/'reads.csv').write_text(original)
    original=(out/'calibration_plan.csv').read_text();(out/'calibration_plan.csv').write_text(original.replace(',integration,',',unknown,',1))
    try:report.summarize(out)
    except ValueError:pass
    else:raise AssertionError('unknown diagnostic kind accepted')
print('Calibration report passed: actual C evidence, 678-frame scope, missing frame, false lock and unknown kind rejection; physical units remain uncalibrated')

for scope,stages,frames in [('gain',14,168),('slow',14,168),('limits',8,78),('digital',6,72),('codes-fine',14,168),('codes-boost',14,168),('digital-headroom',6,72),('limits-fffe',8,78)]:
    with tempfile.TemporaryDirectory() as temporary:
        out=Path(temporary)
        for name in ('calls.csv','reads.csv','frames.csv','calibration_plan.csv','frame_burst.csv','roi.csv','noise.csv'):
            shutil.copyfile(root/'build'/('calibration-'+scope)/name,out/name)
        manifest=f'REQUEST only_sensor=1 first_phase=3 last_phase={stages+2}\nCALIBRATION contract=2 expected_stages={stages} scope={scope}\nSENSOR_END sensor=1 complete=1\nMEASUREMENTS expected={stages} complete={stages} all_complete=1\nEND complete=1 aborted=0 io_error=0\n'
        (out/'manifest.txt').write_text(manifest)
        result=report.summarize(out)
        assert result['measurements_complete'] and result['verified_measured_frames']==frames
        assert result['scope']==scope and result['absolute_iso'] is None
        assert result['expected_valid_measured_frames']==frames
        json.dumps(result)
        original=(out/'frames.csv').read_text();(out/'frames.csv').write_text('\n'.join(original.splitlines()[:-1])+'\n')
        assert not report.summarize(out)['measurements_complete']
        (out/'frames.csv').write_text(original)
        (out/'manifest.txt').write_text(manifest.replace('scope='+scope,'scope=unknown'))
        try:report.summarize(out)
        except ValueError:pass
        else:raise AssertionError('unknown scope accepted')
print('Scoped calibration reports passed: actual gain/slow/limits C evidence, missing frames and unknown scope rejection')

with tempfile.TemporaryDirectory() as temporary:
    out=Path(temporary)
    for name in ('reads.csv','frames.csv','calibration_plan.csv','frame_burst.csv','roi.csv','noise.csv'):
        shutil.copyfile(root/'build/calibration-codes-boost-rejected'/name,out/name)
    (out/'manifest.txt').write_text('REQUEST only_sensor=1 first_phase=3 last_phase=16\nCALIBRATION contract=2 expected_stages=14 scope=codes-boost\nSENSOR_END sensor=1 complete=0\nMEASUREMENTS expected=12 complete=12 all_complete=1\nEND complete=0 aborted=0 io_error=0\n')
    result=report.summarize(out)
    assert not result['measurements_complete'] and result['missing_phases']==[]
    assert result['incomplete_phases']==[13,15] and result['verified_measured_frames']==144
print('Raw-code scan passed: rejected bit encodings remain incomplete; independent subsequent trials require sampled restoration')
result=report.summarize(root/'build/calibration-ram-fixture')
assert result['measurements_complete'] and result['verified_measured_frames']==72
print('Actual C driver using bounded RAM streams passed strict 72-frame evidence verification')
