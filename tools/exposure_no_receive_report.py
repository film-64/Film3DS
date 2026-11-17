#!/usr/bin/env python3
"""Verify no-new-Receive evidence. An absent/stale VSYNC is inconclusive."""
import argparse
import hashlib
import json
from pathlib import Path
import zlib
from decode_exposure_state import read_csv
from exposure_session import manifest_records
from exposure_sweep_contract import CONTRACT_MATRIX, NO_RECEIVE_SOURCE
from exposure_isp_update_report import state, IFP

RULE=CONTRACT_MATRIX[NO_RECEIVE_SOURCE]


def analyse(directory):
    p=Path(directory);receipts=json.loads((p/'received-ram-sha256.json').read_text())
    names=[e['name'] for e in receipts]
    if len(set(names))!=len(names) or not {'manifest.txt','reads.csv','frames.csv','calls.csv'}.issubset(names):
        raise ValueError('missing or duplicate original receipt')
    for e in receipts:
        if Path(e['name']).name!=e['name']:raise ValueError('invalid receipt name')
        b=(p/e['name']).read_bytes()
        if len(b)!=e['bytes'] or hashlib.sha256(b).hexdigest()!=e['sha256'] or f'{zlib.crc32(b):08x}'!=e['crc32']:
            raise ValueError('original evidence integrity failure: '+e['name'])
    if manifest_records(p,'NO_RECEIVE_CONTRACT')!=[dict(source=NO_RECEIVE_SOURCE,arms='2',vsync='3',sensor='1',fps='5',retry='none')]:
        raise ValueError('unregistered no-receive source')
    rows=read_csv(p/'reads.csv');calls=read_csv(p/'calls.csv');frames=read_csv(p/'frames.csv')
    errors=[];arms=[]
    saved_rows=[r for r in rows if r['label']=='isp_update_save' and r['when']=='saved']
    saved={r['addr']:int(r['value']) for r in saved_rows}
    if len(saved_rows)!=3 or set(saved)!={'a102','a355','a11f'} or any(r['sensor']!='1' or r['result']!='00000000' or r['space']!='mcu' for r in saved_rows):
        errors.append('missing_saved_MCU_proof');saved=None
    if any(r['name']=='isp_update_mode' for r in rows):errors.append('unexpected_Mode_reads')
    for index,label in enumerate(('receive_control_128','no_receive_128')):
        phase=5+index;bad=[]
        if dict(index=str(index),phase=str(phase),label=label) not in manifest_records(p,'NO_RECEIVE_ARM'):bad.append('missing_arm_header')
        if dict(index=str(index),phase=str(phase),complete='1') not in manifest_records(p,'NO_RECEIVE_ARM_END'):bad.append('arm_incomplete')
        boundaries=[(-1,'initial')]
        if index:
            boundaries += [(0,'after_dma'),(0,'after_rewrite')]+[(f,'post_vsync') for f in range(1,4)]+[(3,'after_stop')]
        else:
            boundaries += [(0,b) for b in ('before_arm','after_arm','after_start','after_wait','before_rewrite','after_rewrite')]
            boundaries += [(1,'after_wait'),(2,'after_wait'),(2,'after_stop')]
        groups=[state(rows,phase,label,f,b,saved) for f,b in boundaries]
        for g in groups:bad.extend(f"{g['frame']}:{g['boundary']}:{e}" for e in g['errors'])
        selected=[(i,r) for i,r in enumerate(calls) if r['phase']==str(phase)]
        arm_label=lambda r:r['label']==label if index else r['label'].startswith(label+'_f')
        first=[i for i,r in selected if arm_label(r) and r['operation']=='SetReceiving']
        last=[i for i,r in selected if arm_label(r) and r['operation']=='StopCapture']
        start,end=(first[0],last[-1]) if first and last else (-1,-1)
        active=calls[start:end+1] if start>=0 else []
        def positions(op):return [i for i in range(start,end+1) if calls[i]['operation']==op] if start>=0 else []
        counts=dict(SetReceiving=1 if index else 3,StartCapture=1,StopCapture=1,
                    WaitFrame=1 if index else 3,IsFinishedReceiving=1 if index else 3)
        if index:counts.update(CloseDMAEvent=1,ClearVsyncEvent=3,WaitVsync=3,GetLatestVsyncTiming=6)
        for op,count in counts.items():
            chosen=[r for r in active if r['operation']==op]
            if len(chosen)!=count or any(r['result']!='00000000' or r['sensor']!='1' for r in chosen):bad.append('bad_call_count_or_result:'+op)
        if any(r['operation']=='IsFinishedReceiving' and r['value']!='1' for r in active):bad.append('pending_DMA_not_excluded')
        # The window admits no MCU/config writes. Both arms admit only their
        # one registered four-channel perturbation after the first DMA.
        rewrite_label='no_receive_rewrite' if index else 'isp_update_rewrite'
        permitted={'WaitFrame','IsFinishedReceiving','ReadRegisterI2cExclusive','ReadMcuVariableI2cExclusive',
                   'StartCapture','StopCapture','SetReceiving','WriteRegisterI2c'}
        if index:permitted.update(('CloseDMAEvent','ClearVsyncEvent','WaitVsync','GetLatestVsyncTiming'))
        for r in active:
            if r['operation'] not in permitted:bad.append('unexpected_call_in_window:'+r['operation'])
            if r['operation']=='WriteRegisterI2c' and (r['label']!=rewrite_label or r['addr'] not in IFP):bad.append('unexpected_write_in_window')
        rewrites=[i for i,r in selected if r['label']==rewrite_label and r['operation']=='WriteRegisterI2c']
        waits=positions('WaitFrame')
        if len(rewrites)!=4 or not waits or not (waits[0]<min(rewrites)<=max(rewrites)<end):bad.append('rewrite_order_not_verified')
        if not index:
            if len(waits)!=3 or not rewrites or not (max(rewrites)<waits[1]<waits[2]<end):bad.append('control_post_rewrite_DMA_missing')
            next_arm=[i for i in positions('SetReceiving') if calls[i]['label']==label+'_f1']
            if len(next_arm)!=1 or not rewrites or next_arm[0]>min(rewrites):bad.append('control_not_batch13_continuous')
        for write_label in ('isp_update_initial',rewrite_label):
            writes=[r for _,r in selected if r['label']==write_label and r['operation']=='WriteRegisterI2c']
            proof=[r for r in rows if r['phase']==str(phase) and r['label']==write_label and r['when']=='verified']
            for data in (writes,proof):
                if len(data)!=4 or {r['addr'] for r in data}!=set(IFP) or any(r['sensor']!='1' or r['result']!='00000000' or r['value']!='128' for r in data):bad.append('missing_write_or_readback:'+write_label)
        captures=[r for r in frames if r['phase']==str(phase)]
        if len(captures)!=(1 if index else 3) or {r['frame'] for r in captures}!=({'0'} if index else {'0','1','2'}) or any(
                r['sensor']!='1' or r['label']!=label or r['measured']!='0' or r['ok']!='1' or r['received']!='1' or r['result']!='00000000' for r in captures):bad.append('DMA_frames_incomplete')
        vsync_records=[];red=[]
        if index:
            acquired=[r for _,r in selected if r['operation']=='GetVsyncInterruptEvent']
            closed=[r for _,r in selected if r['operation']=='CloseVsyncEvent']
            if len(acquired)!=1 or len(closed)!=1 or acquired[0]['result']!='00000000' or closed[0]['result']!='00000000' or acquired[0]['value']=='0' or acquired[0]['value']!=closed[0]['value']:bad.append('VSYNC_handle_not_verified')
            event=acquired[0]['value'] if len(acquired)==1 else None
            if any(r['value']!=event for r in active if r['operation'] in ('ClearVsyncEvent','WaitVsync')):bad.append('VSYNC_wrong_event')
            if any(r['value']!='1' for r in active if r['operation']=='GetLatestVsyncTiming'):bad.append('VSYNC_wrong_timing_count')
            if any(sum(r['operation']==op for _,r in selected)!=count for op,count in (('SetReceiving',1),('StartCapture',1))):bad.append('extra_receive_or_start_outside_window')
            finished=positions('IsFinishedReceiving');closed_dma=positions('CloseDMAEvent')
            if not (len(finished)==len(closed_dma)==1 and rewrites and waits and waits[0]<finished[0]<closed_dma[0]<min(rewrites)):bad.append('no_pending_DMA_proof_missing')
            elif calls[waits[0]]['value']!=calls[closed_dma[0]]['value']:bad.append('wrong_DMA_handle_closed')
            if manifest_records(p,'NO_RECEIVE_READY')!=[dict(phase='6',pending='0',received='1',dma_closed='1')]:bad.append('no_pending_DMA_record_missing')
            vsync_records=manifest_records(p,'NO_RECEIVE_VSYNC');timings=manifest_records(p,'NO_RECEIVE_TIMING')
            expected_ops=['GetLatestVsyncTiming','ClearVsyncEvent','WaitVsync','GetLatestVsyncTiming']*3
            actual_ops=[r['operation'] for r in active if r['operation'] in set(expected_ops)]
            if actual_ops!=expected_ops:bad.append('VSYNC_clear_wait_timing_order_invalid')
            if len(vsync_records)!=3 or len(timings)!=6:bad.append('new_VSYNC_evidence_incomplete')
            previous=0
            timing_calls=[r for r in active if r['operation']=='GetLatestVsyncTiming']
            for observation in range(1,4):
                v=[r for r in vsync_records if r.get('observation')==str(observation)]
                t=[r for r in timings if r.get('observation')==str(observation)]
                valid=len(v)==1 and len(t)==2
                if valid:
                    baseline=int(v[0].get('baseline','0'));latest=int(v[0].get('latest','0'))
                    valid=v[0].get('verified')=='1' and baseline>0 and latest>baseline and latest>previous
                    valid=valid and t==[dict(observation=str(observation),when='baseline',raw=str(baseline),result='00000000'),dict(observation=str(observation),when='latest',raw=str(latest),result='00000000')]
                    previous=latest
                if not valid:bad.append('new_VSYNC_not_verified:'+str(observation))
                burst=[r for r in rows if r['phase']=='6' and r['label']==label and r['frame']==str(observation) and r['name']=='no_receive_red']
                if len(burst)!=8 or any(r['addr']!='32d4' or r['space']!='reg' or r['sensor']!='1' or r['when']!='post_vsync' or r['result']!='00000000' or not 1<=int(r['value'])<=512 for r in burst):bad.append('red_burst_incomplete:'+str(observation))
                snapshot=next(g for g in groups if g['frame']==observation and g['boundary']=='post_vsync')
                if len(timing_calls)==6 and burst and snapshot['times_ms']:
                    times=[int(r['time_ms']) for r in burst]+snapshot['times_ms']
                    lower=int(timing_calls[2*observation-1]['time_ms'])
                    upper=int(timing_calls[2*observation]['time_ms']) if observation<3 else int(calls[end]['time_ms'])
                    if times!=sorted(times) or min(times)<lower or max(times)>upper:bad.append('post_VSYNC_read_order_invalid:'+str(observation))
                red.append([dict(value=int(r['value']),time_ms=int(r['time_ms'])) for r in burst])
            if manifest_records(p,'NO_RECEIVE_WINDOW')!=[dict(complete='1',observations='3',raw_provenance='first_DMA_only')]:bad.append('window_incomplete')
        raw=p/f's1-p{phase:03d}-{label}.yuyv'
        raw_records=[r for r in manifest_records(p,'SWEEP_RAW') if r.get('phase')==str(phase)]
        if raw.name not in names or not raw.is_file() or raw.stat().st_size!=24576 or len(raw_records)!=1 or raw_records[0].get('crc32')!=f'{zlib.crc32(raw.read_bytes()):08x}':bad.append('ROI_missing_or_changed')
        observed={f'{f}:{b}':[g['values'].get(a) for a in IFP] for (f,b),g in zip(boundaries,groups)}
        final=observed['3:post_vsync' if index else '2:after_wait']
        outcome='inconclusive' if bad else 'rewrite_held' if final==[128]*4 else 'rewrite_overwritten'
        arms.append(dict(index=index,phase=phase,label=label,outcome=outcome,observed=observed,groups=groups,vsync=vsync_records,red_bursts=red,
                         raw_provenance='first_DMA_only' if index else 'last_DMA',errors=bad))
        errors.extend(f'arm{index}:{e}' for e in bad)
    commands=[r for r in calls if r['operation']=='WriteMcuVariableI2c' and r['addr']=='a103']
    if len(commands)!=3 or [r['label'] for r in commands]!=['sweep_refresh_apply']*2+['sweep_refresh_restore'] or any(r['value']!='5' or r['result']!='00000000' for r in commands):errors.append('refresh_budget_or_proof_failed')
    refreshed=manifest_records(p,'SEQ_REFRESH')
    if len(refreshed)!=3 or [r.get('restoring') for r in refreshed]!=['0','0','1'] or any(r.get('verified')!='1' or r.get('state')!='3' or not 2<=int(r.get('polls','0'))<=100 for r in refreshed):errors.append('refresh_completion_not_verified')
    for r in calls:
        if r['operation']=='WriteMcuVariableI2c' and r['addr'] not in ('a102','a103','a11f','a355'):errors.append('unregistered_MCU_write:'+r['addr'])
    for label in ('PREVIEW_AWB_RESTORE','SEQ_RESTORE','AWB_RESTORE'):
        records=manifest_records(p,label);mask=96 if label=='AWB_RESTORE' else 65535
        if len(records)!=1 or records[0].get('verified')!='1' or (int(records[0].get('saved','-1'))&mask)!=(int(records[0].get('actual','-2'))&mask):errors.append('restore_not_verified:'+label)
    for label in ('sweep_original','sweep_restore_controls'):
        selected=[r for r in rows if r['label']==label and r['name']=='calibration_saved']
        if len(selected)!=5 or any(r['result']!='00000000' for r in selected):errors.append('physical_restore_proof_missing:'+label)
        values={r['addr']:r['value'] for r in selected}
        if label=='sweep_original':original=values
        elif values!=original:errors.append('physical_controls_not_restored')
    if manifest_records(p,'NO_RECEIVE_END')!=[dict(complete='1',arms='2',restored='1')]:errors.append('experiment_incomplete')
    end=manifest_records(p,'END')
    if len(end)!=1 or any(end[0].get(k)!=v for k,v in dict(complete='1',aborted='0',io_error='0').items()):errors.append('run_incomplete')
    conclusion='inconclusive'
    if not errors:
        nr=arms[1]['observed']['3:post_vsync'];control=arms[0]['observed']['2:after_wait']
        conclusion='new_SetReceiving_not_necessary' if nr!=[128]*4 else 'Receive_related_path_not_excluded' if control!=[128]*4 else 'overwrite_not_reproduced'
    return dict(schema='no-receive-frame-update-report/v1',source=NO_RECEIVE_SOURCE,passed=not errors,calibration_validated=False,
                atomic_snapshot=False,files_verified=len(receipts),errors=errors,arms=arms,conclusion=conclusion,
                writer_identified=False,timestamp_units='raw_service_value')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();result=analyse(args.directory)
    with args.out.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps(dict(passed=result['passed'],files=result['files_verified'],errors=result['errors'],conclusion=result['conclusion'],arms=[dict(label=a['label'],outcome=a['outcome'],observed=a['observed'],vsync=a['vsync']) for a in result['arms']]),ensure_ascii=False))
    raise SystemExit(0 if result['passed'] else 2)
