#!/usr/bin/env python3
"""Verify calibration-1 evidence without inventing ISO, exposure seconds or optical aperture."""
import argparse,csv,json,math,re,statistics
from collections import defaultdict
from pathlib import Path
from exposure_report import PHYSICAL,success

CONTROLS=('3012','3056','3058','305a','305c')
FIELDS=PHYSICAL+('3002','3004','3006','3008','3028','302a','302c','302e','3030')
MASKS={'seed':{0},'normalize':{30},'integration':{1},'integration_long':{1},'analog':{30},'digital':{30},'restore':{1,30},'code_baseline':{31},'code':{30}}

def rows(directory,name):
    with (directory/name).open(newline='') as file:
        result=list(csv.DictReader(file))
    if any(None in r or any(v is None for v in r.values()) for r in result):raise ValueError('truncated CSV: '+name)
    return result

def summarize(directory):
    directory=Path(directory);manifest=(directory/'manifest.txt').read_text()
    plan=rows(directory,'calibration_plan.csv');frames=rows(directory,'frames.csv');reads=rows(directory,'reads.csv')
    roi=rows(directory,'roi.csv');noise=rows(directory,'noise.csv');bursts=rows(directory,'frame_burst.csv')
    scoped=re.search(r'REQUEST only_sensor=([123]) first_phase=3 last_phase=(3|8|10|16|60)\n',manifest)
    if not scoped:raise ValueError('wrong calibration request')
    declaration=re.search(r'CALIBRATION contract=([12]) expected_stages=(\d+) [^\n]*scope=(smoke|full|gain|slow|limits|digital|codes-fine|codes-boost|digital-headroom|limits-fffe)\n',manifest)
    if not declaration:raise ValueError('unknown calibration scope')
    scope=declaration[3];smoke=scope=='smoke'
    expected_stages,expected_total_frames,rates={'smoke':(1,12,()),'full':(58,678,('8','0','5')),'gain':(14,168,('0',)),'slow':(14,168,('5',)),'limits':(8,78,()),'digital':(6,72,('0',)),'codes-fine':(14,168,()),'codes-boost':(14,168,()),'digital-headroom':(6,72,('0',)),'limits-fffe':(8,78,())}[scope]
    last=expected_stages+2
    if int(scoped[2])!=last or int(declaration[2])!=expected_stages or int(declaration[1])!=(1 if scope in ('smoke','full') else 2):raise ValueError('wrong scope/stage contract')
    sensor=scoped[1];phases={};bad=[];seen=set()
    if any(r['sensor']==sensor and r['measured']=='1' and not 3<=int(r['phase'])<=last for r in frames):raise ValueError('measured work outside declared scope')
    grouped=[defaultdict(list) for _ in range(4)]
    for source,group in zip((frames,reads,roi,noise),grouped):
        for row in source:
            if row['sensor']==sensor:group[row['phase']].append(row)
    for stage in plan:
        phase=stage['phase'];kind=stage['kind'];mask=int(stage['write_mask'])
        if phase in seen or stage['sensor']!=sensor or kind not in MASKS or mask not in MASKS[kind]:raise ValueError('duplicate/unknown stage or write mask')
        seen.add(phase);settle=int(stage['settle']);n=int(stage['measured'])
        if (settle,n)!=((1,3) if kind=='integration_long' else (8,12)):raise ValueError('wrong declared frame contract')
        expected_frames={str(i) for i in range(settle,settle+n)}
        fs=[f for f in grouped[0][phase] if f['measured']=='1']
        frame_ok=len(fs)==n and {f['frame'] for f in fs}==expected_frames and all(f['ok']=='1' and f['received']=='1' and success(f) for f in fs)
        rs=grouped[1][phase];complete=True;registers={}
        for addr in FIELDS:
            selected=[r for r in rs if r['space']=='reg' and r['addr']==addr and r['when']=='after' and r['frame'] in expected_frames]
            valid=[r for r in selected if success(r)]
            ok=len(selected)==n and len(valid)==n and {r['frame'] for r in valid}==expected_frames
            complete=complete and ok
            values=[int(r['value']) for r in valid]
            registers[addr]={'complete':ok,'values':values,'constant':ok and len(set(values))==1}
        locked=stage['locked']=='1';held=True
        if locked:
            targets=[int(stage[k]) for k in ('coarse','gain_gr','gain_b','gain_r','gain_gb')]
            for addr,target in zip(CONTROLS,targets):
                for when in ('before','after'):
                    matched=[r for r in rs if r['space']=='reg' and r['addr']==addr and r['when']==when and r['frame'] in expected_frames]
                    held=held and len(matched)==n and len({r['frame'] for r in matched})==n and all(success(r) and int(r['value'])==target for r in matched)
            for addr in ('a11d','a11f'):
                for when in ('before','after'):
                    matched=[r for r in rs if r['space']=='mcu' and r['addr']==addr and r['when']==when and r['frame'] in expected_frames]
                    held=held and len(matched)==n and len({r['frame'] for r in matched})==n and all(success(r) and int(r['value'])==0 for r in matched)
        central=[r for r in grouped[2][phase] if r['tile']=='0' and r['frame'] in expected_frames]
        roi_ok=len(central)==n and {r['frame'] for r in central}==expected_frames
        centers=[float(r['mean']) for r in central];sigmas=grouped[3][phase]
        noise_ok=len(sigmas)==1 and int(sigmas[0]['frames'])==n and float(sigmas[0]['temporal_variance'])>=0
        stage_ok=frame_ok and complete and held and roi_ok and noise_ok
        if not stage_ok:bad.append(int(phase))
        phases[phase]={'label':stage['label'],'kind':kind,'expected_measured':n,'measured_rows':len(fs),
            'frames_complete':frame_ok,'register_fields_complete':complete,'locked_controls_held':held,
            'roi_complete':roi_ok,'noise_complete':noise_ok,'complete':stage_ok,'registers':registers,
            'center_mean':statistics.median(centers) if centers else None,
            'center_mean_span':max(centers)-min(centers) if centers else None,
            'center_high_fraction':statistics.mean(int(r['high'])/(int(r['width'])*int(r['height'])) for r in central) if central else None,
            'processed_temporal_sigma':float(sigmas[0]['temporal_sigma']) if noise_ok else None,
            'baseline_phase':int(stage['baseline_phase'])}
    cadence={}
    if any(r['sensor']!=sensor or r['rate'] not in rates for r in bursts):raise ValueError('DMA burst outside declared scope')
    for rate in rates:
        bs=[r for r in bursts if r['sensor']==sensor and r['rate']==rate]
        ok=len(bs)==24 and {int(r['frame']) for r in bs}==set(range(24)) and all(r['wait_result']=='00000000' and success({'result':r['receive_result']}) and r['received']=='1' and r['buffer_error']=='0' for r in bs)
        times=[int(r['time_ms']) for r in sorted(bs,key=lambda r:int(r['frame']))]
        intervals=[b-a for a,b in zip(times[2:],times[3:])]
        ok=ok and bool(intervals) and min(intervals)>0
        cadence[rate]={'complete':ok,'interval_ms':intervals,'median_interval_ms':statistics.median(intervals) if intervals else None,
            'meaning':'DMA completion event cadence; not independently calibrated integration time'}
    missing=sorted(set(range(3,last+1))-{int(p) for p in seen})
    complete=(not missing and len(plan)==expected_stages and not bad and f'SENSOR_END sensor={sensor} complete=1' in manifest
              and f'MEASUREMENTS expected={expected_stages} complete={expected_stages} all_complete=1' in manifest and 'END complete=1 aborted=0 io_error=0' in manifest)
    kinds=[p['kind'] for p in phases.values()]
    expected_kinds={'smoke':{'seed':1},'full':{'seed':3,'normalize':3,'integration':18,'restore':26,'analog':4,'digital':2,'integration_long':2},'gain':{'seed':1,'normalize':1,'analog':4,'digital':2,'restore':6},'slow':{'seed':1,'normalize':1,'integration':6,'restore':6},'limits':{'seed':2,'normalize':2,'integration_long':2,'restore':2},'digital':{'seed':1,'normalize':1,'digital':2,'restore':2},'codes-fine':{'seed':1,'code_baseline':1,'code':6,'restore':6},'codes-boost':{'seed':1,'code_baseline':1,'code':6,'restore':6},'digital-headroom':{'seed':1,'code_baseline':1,'digital':2,'restore':2},'limits-fffe':{'seed':2,'normalize':2,'integration_long':2,'restore':2}}[scope]
    if any(kinds.count(k)>n for k,n in expected_kinds.items()) or any(k not in expected_kinds for k in kinds):raise ValueError('stage kinds outside declared scope')
    complete=complete and all(kinds.count(k)==n for k,n in expected_kinds.items())
    return {'schema':'filmprobe-calibration-'+declaration[1],'sensor':sensor,'scope':scope,'measurements_complete':complete,
            'timing_bursts_complete':None if not rates else all(v['complete'] for v in cadence.values()),'missing_phases':missing,'incomplete_phases':bad,
            'expected_valid_measured_frames':expected_total_frames,'verified_measured_frames':sum(p['expected_measured'] for p in phases.values() if p['complete']),
            'phases':phases,'cadence':cadence,'absolute_iso':None,'absolute_shutter_seconds':None,'f_number':None,
            'limits':['Raw control-code measurements and processed Y noise; no RAW electron-noise claim.',
            'Metal target specular reflections, drift and output clipping must be reviewed.',
            'A finite coarse-register boundary is not proof of B-mode or a calibrated shutter range.',
            'Cadence and single-frame integration measurements are separate proofs.']}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);args=p.parse_args()
    result=summarize(args.directory);(args.directory/'calibration-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Measurements complete:',result['measurements_complete'],'valid measured frames:',result['verified_measured_frames'])
    print('Timing bursts complete:',result['timing_bursts_complete'],'incomplete phases:',result['incomplete_phases'])
if __name__=='__main__':main()
