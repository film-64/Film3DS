#!/usr/bin/env python3
"""Verify registered ISP update observations, never a unity calibration."""
import argparse
import hashlib
import json
from pathlib import Path
import zlib
from decode_exposure_state import read_csv
from exposure_session import manifest_records
from exposure_sweep_contract import CONTRACT_MATRIX, UPDATE_SOURCE, UPDATE_ARMS

RULE=CONTRACT_MATRIX[UPDATE_SOURCE]
REGISTERS=RULE['observation']['registers']
MODE=RULE['observation']['mode_registers']
IFP=['32d4','32d6','32d8','32da']


def state(rows,phase,label,frame,when,saved=None):
    selected=[r for r in rows if r['phase']==str(phase) and r['label']==label and r['frame']==str(frame)
              and r['when']==when and r['name']=='isp_update_state']
    values={};errors=[]
    for r in selected:
        a=r['addr']
        if a in values:errors.append('duplicate:'+a)
        values[a]=int(r['value'])
        if r['sensor']!='1' or r['result']!='00000000' or r['space']!=('mcu' if a.startswith('a') or a=='221f' else 'reg'):
            errors.append('invalid_read:'+a)
    if set(values)!=set(REGISTERS):errors.append('missing_or_unexpected_register')
    expected=dict(a103=0,a104=3,a34e=128,a34f=128,a350=128)
    expected.update({'3012':4,**{a:528 for a in ('3056','3058','305a','305c')}})
    if not errors:
        if values['a102']&5:errors.append('drivers_not_disabled')
        if not values['a355']&32:errors.append('force_unity_not_set')
        if saved and values['a102']!=(saved['a102']&~5):errors.append('sequencer_mode_changed')
        if saved and (values['a355']&96)!=((saved['a355']|32)&96):errors.append('AWB_control_changed')
        for a,v in expected.items():
            if values[a]!=v:errors.append('control_drift:'+a)
        if any(not 1<=values[a]<=512 for a in IFP):errors.append('invalid_IFP_range')
    return dict(frame=frame,boundary=when,values=values,errors=errors,
                times_ms=[int(r['time_ms']) for r in selected])


def analyse(directory):
    p=Path(directory);receipts=json.loads((p/'received-ram-sha256.json').read_text())
    names=[e['name'] for e in receipts]
    if len(set(names))!=len(names) or not {'manifest.txt','reads.csv','frames.csv','calls.csv'}.issubset(names):
        raise ValueError('missing or duplicate original receipt')
    for e in receipts:
        name=e['name']
        if Path(name).name!=name:raise ValueError('invalid receipt name')
        b=(p/name).read_bytes()
        if len(b)!=e['bytes'] or hashlib.sha256(b).hexdigest()!=e['sha256'] or f'{zlib.crc32(b):08x}'!=e['crc32']:
            raise ValueError('original evidence integrity failure: '+name)
    expected=dict(source=UPDATE_SOURCE,arms='4',frames='3',sensor='1',fps='5',retry='none',context_writes='0',mcu_ram_writes='0')
    if manifest_records(p,'ISP_UPDATE_CONTRACT')!=[expected]:raise ValueError('unregistered update source')
    rows=read_csv(p/'reads.csv');calls=read_csv(p/'calls.csv');frames=read_csv(p/'frames.csv')
    headers=manifest_records(p,'ISP_UPDATE_ARM');ends=manifest_records(p,'ISP_UPDATE_ARM_END')
    errors=[];arms=[]
    saved_rows=[r for r in rows if r['label']=='isp_update_save' and r['when']=='saved']
    saved={r['addr']:int(r['value']) for r in saved_rows}
    if len(saved_rows)!=3 or set(saved)!={'a102','a355','a11f'} or any(
            r['sensor']!='1' or r['space']!='mcu' or r['result']!='00000000' for r in saved_rows):
        errors.append('missing_saved_MCU_proof');saved=None
    if len(headers)!=4 or len(ends)!=4:errors.append('missing_arm_header_or_end')
    for index,(mode,initial) in enumerate(UPDATE_ARMS):
        phase=5+index;label=f'isp_update_{mode}_{initial:03d}';continuous=mode=='continuous'
        header=dict(index=str(index),phase=str(phase),label=label,initial=str(initial),continuous=str(int(continuous)))
        arm_errors=[]
        if header not in headers:arm_errors.append('missing_arm_header')
        if dict(index=str(index),phase=str(phase),complete='1') not in ends:arm_errors.append('arm_incomplete')
        groups=[state(rows,phase,label,-1,'initial',saved)]
        for frame in range(3):
            boundaries=['after_wait']
            if frame==0 or not continuous:boundaries=['before_arm','after_arm','after_start']+boundaries
            if not continuous or frame==2:boundaries+=['after_stop']
            if frame==0:boundaries+=['before_rewrite','after_rewrite']
            groups.extend(state(rows,phase,label,frame,b,saved) for b in boundaries)
        for group in groups:arm_errors.extend(f"{group['frame']}:{group['boundary']}:{e}" for e in group['errors'])
        captures=[r for r in frames if r['phase']==str(phase)]
        if len(captures)!=3 or {r['frame'] for r in captures}!={'0','1','2'} or any(
                r['sensor']!='1' or r['label']!=label or r['measured']!='0' or r['ok']!='1' or
                r['result']!='00000000' or r['received']!='1' for r in captures):arm_errors.append('DMA_frames_incomplete')
        arm_calls=[r for r in calls if r['phase']==str(phase)]
        point_calls=[r for r in arm_calls if r['label'].startswith(label+'_f')]
        point_positions=[i for i,r in enumerate(calls) if r['phase']==str(phase) and r['label'].startswith(label+'_f')]
        first_receive=[i for i in point_positions if calls[i]['operation']=='SetReceiving']
        last_stop=[i for i in point_positions if calls[i]['operation']=='StopCapture']
        window=(first_receive[0],last_stop[-1]) if first_receive and last_stop else (-1,-1)
        active_calls=calls[window[0]:window[1]+1] if window[0]>=0 else []
        for operation,count in [('StartCapture',1 if continuous else 3),('StopCapture',1 if continuous else 3),('SetReceiving',3),('WaitFrame',3),('IsFinishedReceiving',3)]:
            selected=[r for r in active_calls if r['operation']==operation]
            if len(selected)!=count or any(r['result']!='00000000' for r in selected):arm_errors.append('bad_call_count_or_result:'+operation)
        positions=lambda op,lab=None:[i for i,r in enumerate(calls) if window[0]<=i<=window[1] and r['operation']==op and (lab is None or r['label']==lab)]
        first_wait=positions('WaitFrame',label+'_f0');next_wait=positions('WaitFrame',label+'_f1')
        stops=positions('StopCapture');starts=positions('StartCapture');receiving=positions('SetReceiving')
        rewritten=[i for i,r in enumerate(calls) if r['phase']==str(phase) and r['label']=='isp_update_rewrite' and r['operation']=='WriteRegisterI2c']
        initial_writes=[i for i,r in enumerate(calls) if r['phase']==str(phase) and r['label']=='isp_update_initial' and r['operation']=='WriteRegisterI2c']
        if len(rewritten)!=4 or not first_wait or not next_wait or not receiving or not initial_writes or not (
                max(initial_writes)<min(receiving) and first_wait[0]<min(rewritten)<=max(rewritten)<next_wait[0]):
            arm_errors.append('rewrite_or_initial_order_not_verified')
        elif continuous:
            next_arm=positions('SetReceiving',label+'_f1')
            if len(starts)!=1 or len(stops)!=1 or stops[0]<max(positions('WaitFrame')) or len(next_arm)!=1 or next_arm[0]>min(rewritten):
                arm_errors.append('continuous_rewrite_not_within_same_stream')
        elif len(stops)!=3 or len(starts)!=3 or not (stops[0]<min(rewritten)<=max(rewritten)<starts[1]):
            arm_errors.append('restart_rewrite_not_between_stop_and_start')
        if point_calls:
            span=[i for i,r in enumerate(calls) if r in point_calls]
            if any(calls[i]['operation']=='WriteMcuVariableI2c' and calls[i]['addr']=='a103' for i in range(min(span),max(span)+1)):
                arm_errors.append('refresh_during_arm')
        unexpected=[r for r in arm_calls if r['operation']=='WriteRegisterI2c' and r['addr'] in IFP and r['label'] not in ('isp_update_initial','isp_update_rewrite','sweep_restore_isp')]
        if unexpected:arm_errors.append('unregistered_IFP_rewrite')
        for write_label,value in [('isp_update_initial',initial),('isp_update_rewrite',128)]:
            writes=[r for r in arm_calls if r['label']==write_label and r['operation']=='WriteRegisterI2c']
            verified=[r for r in rows if r['phase']==str(phase) and r['label']==write_label and r['when']=='verified']
            if len(writes)!=4 or {r['addr'] for r in writes}!=set(IFP) or any(
                    r['sensor']!='1' or r['value']!=str(value) or r['result']!='00000000' for r in writes):arm_errors.append('invalid_write:'+write_label)
            if len(verified)!=4 or {r['addr'] for r in verified}!=set(IFP) or any(
                    r['sensor']!='1' or r['value']!=str(value) or r['result']!='00000000' for r in verified):arm_errors.append('invalid_write_proof:'+write_label)
        for frame,when in [(-1,'before_initial'),(0,'after_frame0'),(2,'after_frame2')]:
            selected=[r for r in rows if r['phase']==str(phase) and r['label']==label and r['frame']==str(frame) and r['when']==when and r['name']=='isp_update_mode']
            if len(selected)!=len(MODE) or {r['addr'] for r in selected}!=set(MODE) or any(
                    r['sensor']!='1' or r['result']!='00000000' or r['space']!='mcu' for r in selected):arm_errors.append('incomplete_mode_snapshot:'+when)
        raw=p/f's1-p{phase:03d}-{label}.yuyv'
        raw_records=[r for r in manifest_records(p,'SWEEP_RAW') if r.get('phase')==str(phase)]
        if raw.name not in names or not raw.is_file() or raw.stat().st_size!=24576 or len(raw_records)!=1 or raw_records[0].get('crc32')!=f'{zlib.crc32(raw.read_bytes()):08x}':
            arm_errors.append('missing_or_changed_last_frame_ROI')
        observed={}
        for frame,when,key in [(0,'after_wait','first_frame'),(0,'before_rewrite','before_rewrite'),(0,'after_rewrite','rewrite'),(1,'after_wait','next_frame'),(2,'after_wait','fresh_frame')]:
            group=next(g for g in groups if g['frame']==frame and g['boundary']==when)
            observed[key]=[group['values'].get(a) for a in IFP]
        fresh=observed['fresh_frame']
        outcome='inconclusive' if arm_errors else 'rewrite_held' if fresh==[128]*4 else 'rewrite_overwritten'
        arms.append(dict(index=index,phase=phase,mode=mode,initial=initial,observed=observed,outcome=outcome,errors=arm_errors,groups=groups))
        errors.extend(f'arm{index}:{e}' for e in arm_errors)
    commands=[r for r in calls if r['operation']=='WriteMcuVariableI2c' and r['addr']=='a103']
    if len(commands)!=5 or [r['label'] for r in commands]!=['sweep_refresh_apply']*4+['sweep_refresh_restore'] or any(
            r['value']!='5' or r['result']!='00000000' for r in commands):errors.append('refresh_budget_or_proof_failed')
    refreshed=manifest_records(p,'SEQ_REFRESH')
    if len(refreshed)!=5 or [r.get('restoring') for r in refreshed]!=['0']*4+['1'] or any(
            r.get('verified')!='1' or r.get('state')!='3' or not 2<=int(r.get('polls','0'))<=100 for r in refreshed):errors.append('refresh_completion_not_verified')
    for r in calls:
        if r['operation']=='WriteMcuVariableI2c' and r['addr'] not in ('a102','a103','a11f','a355'):errors.append('unregistered_MCU_write:'+r['addr'])
    for label in ('PREVIEW_AWB_RESTORE','SEQ_RESTORE','AWB_RESTORE'):
        records=manifest_records(p,label);mask=96 if label=='AWB_RESTORE' else 65535
        if len(records)!=1 or records[0].get('verified')!='1' or (int(records[0].get('saved','-1'))&mask)!=(int(records[0].get('actual','-2'))&mask):errors.append('restore_not_verified:'+label)
    if manifest_records(p,'ISP_UPDATE_END')!=[dict(complete='1',arms='4',restored='1')]:errors.append('experiment_incomplete')
    end=manifest_records(p,'END')
    if len(end)!=1 or any(end[0].get(k)!=v for k,v in dict(complete='1',aborted='0',io_error='0').items()):errors.append('run_incomplete')
    return dict(schema='exposure-isp-update-report/v1',source=UPDATE_SOURCE,passed=not errors,
                calibration_validated=False,atomic_snapshot=False,files_verified=len(receipts),
                errors=errors,arms=arms,mode_values_matching_133=[dict(phase=r['phase'],when=r['when'],addr=r['addr']) for r in rows if r['name']=='isp_update_mode' and r['value']=='133'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();result=analyse(args.directory)
    with args.out.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps(dict(passed=result['passed'],files=result['files_verified'],errors=len(result['errors']),arms=[dict(mode=a['mode'],initial=a['initial'],observed=a['observed'],outcome=a['outcome']) for a in result['arms']]),ensure_ascii=False))
    raise SystemExit(0 if result['passed'] else 2)
