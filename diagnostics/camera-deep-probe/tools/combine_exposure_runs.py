#!/usr/bin/env python3
"""Combine explicitly scoped measurements while retaining both original runs and provenance."""
import argparse,csv,copy,hashlib,json,shutil
from pathlib import Path
import exposure_report

def verified(directory):
    directory=Path(directory)
    records=json.loads((directory/'received-sha256.json').read_text())
    for record in records:
        path=directory/record['name']
        if path.stat().st_size!=record['bytes'] or hashlib.sha256(path.read_bytes()).hexdigest()!=record['sha256']:
            raise ValueError(f'changed/incomplete source: {path}')
    return records

def combine(original,continuation,out):
    original,continuation,out=map(Path,(original,continuation,out))
    verified(original);verified(continuation)
    old=exposure_report.summarize(original);new=exposure_report.summarize(continuation)
    request=new['request'];sensor=str(int(request['only_sensor']));first=int(request['first_phase'])
    if sensor!='3' or first!=34 or int(request['last_phase'])!=72 or not new['run_complete']:
        raise ValueError('continuation must completely cover the explicit sensor 3, phase 34..72 request')
    if any(f'SENSOR_END sensor={s} complete=1' not in old['manifest'] for s in ('1','2')):
        raise ValueError('original sensors 1 and 2 must have explicit completed records')
    sensors=copy.deepcopy(old['sensors'])
    merged=sensors[sensor];extra=new['sensors'][sensor]
    merged['phases']={**{k:v for k,v in merged['phases'].items() if int(k)<first},
                      **{k:v for k,v in extra['phases'].items() if int(k)>=first}}
    merged['exposure_steps']=[r for r in merged['exposure_steps'] if r['phase']<first]+[r for r in extra['exposure_steps'] if r['phase']>=first]
    merged['direct_trials']=[r for r in merged['direct_trials'] if r['phase']<first]+[r for r in extra['direct_trials'] if r['phase']>=first]
    merged['chip_ids']=sorted(set(merged['chip_ids'])|set(extra['chip_ids']))
    merged['capabilities_by_source']={'original':merged.pop('capabilities'),'continuation':extra['capabilities']}
    coverage={}
    for s,data in sensors.items():
        phases=data['phases']
        missing=[p for p in range(3,73) if not phases.get(str(p),{}).get('frames_complete')]
        incomplete=[p for p in range(3,73) if not all(phases.get(str(p),{}).get('registers',{}).get(a,{}).get('complete',False) for a in exposure_report.PHYSICAL)]
        coverage[s]={'expected_stages':70,'incomplete_frame_phases':missing,'incomplete_register_phases':incomplete}
    if any(c['incomplete_frame_phases'] or c['incomplete_register_phases'] for c in coverage.values()):
        raise ValueError(f'incomplete combined evidence: {coverage}')
    out.mkdir(parents=True,exist_ok=False)
    def selected(row,source):
        if source=='original':return row['sensor']!='3' or int(row['phase'])<first
        return row['sensor']=='3' and int(row['phase'])>=first
    source_records=[]
    for directory,name in ((original,'original'),(continuation,'continuation')):
        source_records.append({'id':name,'path':str(directory.resolve()),
            'manifest':(directory/'manifest.txt').read_text(),
            'sha256':{file:hashlib.sha256((directory/file).read_bytes()).hexdigest() for file in ('calls.csv','reads.csv','frames.csv','manifest.txt','ipc.csv')}})
    for file in ('calls.csv','reads.csv','frames.csv'):
        with (out/file).open('w',newline='') as target:
            writer=None
            for directory,name in ((original,'original'),(continuation,'continuation')):
                with (directory/file).open(newline='') as f:
                    reader=csv.DictReader(f)
                    if writer is None:
                        writer=csv.DictWriter(target,fieldnames=reader.fieldnames+['source_run']);writer.writeheader()
                    for row in reader:
                        if selected(row,name):writer.writerow({**row,'source_run':name})
    for directory,name in ((original,'original'),(continuation,'continuation')):
        for path in directory.glob('s*-p*.pgm'):
            s,p=path.name.split('-',2)[:2]
            if selected({'sensor':s[1:],'phase':p[1:]},name):shutil.copyfile(path,out/path.name)
    result={'schema':'filmprobe-combined-1','dataset_complete':True,'coverage':coverage,'sources':source_records,
            'sensors':sensors,'limits':['Sensor 3 spans two runs and is not one continuous recording.',
            'No raw source is replaced; interrupted phase 34 remains in the original run.',
            'Completeness does not turn a failed control experiment into a passing one.',
            'Registers are raw codes, not calibrated shutter seconds or ISO.']}
    (out/'analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('original',type=Path);p.add_argument('continuation',type=Path);p.add_argument('--out',required=True,type=Path)
    args=p.parse_args();result=combine(args.original,args.continuation,args.out)
    print('Complete combined data:',sum(c['expected_stages'] for c in result['coverage'].values()),'stages; sources retained separately')
if __name__=='__main__':main()
