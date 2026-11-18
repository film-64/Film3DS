#!/usr/bin/env python3
"""Plot measured step sweeps; sensor register codes remain explicitly uncalibrated."""
import argparse,csv,json,statistics
from collections import defaultdict
from pathlib import Path

def summarize(directory):
    directory=Path(directory);analysis=json.loads((directory/'analysis.json').read_text())
    mcu=defaultdict(list)
    with (directory/'reads.csv').open(newline='') as f:
        for row in csv.DictReader(f):
            if row['space']=='mcu' and row['addr']=='a206' and row['when']=='after' and row['result']=='00000000':
                mcu[(row['sensor'],row['phase'])].append(int(row['value']))
    rows=[]
    for sensor,data in analysis['sensors'].items():
        for phase,s in data['phases'].items():
            label=s['label']
            if not label.startswith(('ae0_exp_','ae1_exp_')) or not s['frames_complete']:continue
            requested=label.rsplit('_',1)[1];step=int(requested[1:])*(-1 if requested[0]=='m' else 1)
            def register(addr):
                values=s['registers'][addr]['values'];return statistics.median(values) if values else None
            target=mcu[(sensor,phase)]
            rows.append({'sensor':sensor,'phase':phase,'ae':int(label[2]),'requested_step':step,
                         'y_mean':s['y_mean'],'coarse_code':register('3012'),'fine_code':register('3014'),
                         'green_gain_code':register('3056'),'digital_green_code':register('3032'),
                         'ae_target_code':statistics.median(target) if target else None,
                         'physical_state':s['physical_state']})
    fields=['sensor','phase','ae','requested_step','y_mean','coarse_code','fine_code','green_gain_code','digital_green_code','ae_target_code','physical_state']
    with (directory/'exposure-step-summary.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(rows)
    return rows

def plot(directory,rows):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    for sensor in sorted({r['sensor'] for r in rows}):
        fig,axes=plt.subplots(2,2,figsize=(10,7))
        for ax,(field,label) in zip(axes.flat,[('y_mean','Output Y mean (code)'),('coarse_code','Coarse integration register code'),('green_gain_code','Green combined-gain register code'),('ae_target_code','Internal AE target code')]):
            for ae in (0,1):
                groups=defaultdict(list)
                for r in rows:
                    if r['sensor']==sensor and r['ae']==ae and r[field] is not None:groups[r['requested_step']].append(r[field])
                xs=sorted(groups);ys=[statistics.median(groups[x]) for x in xs]
                lower=[y-min(groups[x]) for x,y in zip(xs,ys)];upper=[max(groups[x])-y for x,y in zip(xs,ys)]
                if xs:ax.errorbar(xs,ys,yerr=[lower,upper],marker='o',label=f'AE {"on" if ae else "off"} requested before sweep',capsize=3)
            ax.set(xlabel='Requested SetExposure step',ylabel=label);ax.grid(alpha=.25);ax.legend()
        fig.suptitle(f'Sensor {sensor}: measured exposure sweep\nRegister codes are not shutter seconds or ISO; bars show repeated-stage ranges')
        fig.tight_layout();fig.savefig(Path(directory)/f'sensor-{sensor}-exposure-curves.png',dpi=160);plt.close(fig)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);args=p.parse_args()
    rows=summarize(args.directory);plot(args.directory,rows);print('Exported',len(rows),'measured step stages')
if __name__=='__main__':main()
