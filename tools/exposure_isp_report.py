#!/usr/bin/env python3
"""Read-only check/v4 MCU/IFP evidence verification; never a calibration model."""
import argparse
import hashlib
import json
from pathlib import Path
import zlib
from decode_exposure_state import read_csv
from exposure_session import manifest_records
from exposure_sweep_contract import CONTRACT_MATRIX, CHECK_SOURCE, REFRESH_SOURCE, MANUAL_SOURCE

SOURCE=CHECK_SOURCE
MCU=CONTRACT_MATRIX[SOURCE]['transition_probe']['mcu_registers']
IFP=['32d4','32d6','32d8','32da']
BOUNDARIES=CONTRACT_MATRIX[SOURCE]['transition_probe']['boundaries']


def group(rows, mcu=None, registers=None):
    mcu=MCU if mcu is None else mcu
    registers=IFP if registers is None else registers
    values={};errors=[]
    for r in rows:
        key=(r['space'],r['addr'])
        if key in values:errors.append('duplicate:'+r['addr'])
        values[key]=int(r['value'])
        if int(r['result'],16)!=0:errors.append('read_failed:'+r['addr'])
    required={('mcu',a) for a in mcu}|{('reg',a) for a in registers}
    if set(values)!=required:errors.append('missing_or_unexpected_register')
    if any(r.get('sensor')!='1' or r.get('label')!='sweep_s13_c0004' for r in rows):errors.append('wrong_sensor_or_label')
    return {a:values.get(('mcu' if a in mcu else 'reg',a)) for a in mcu+registers},errors


def measured(rows, source=SOURCE):
    if source not in CONTRACT_MATRIX['exposure-isp-diagnostic-report/v3']['input_sources']:
        raise ValueError('unregistered diagnostic source')
    mcu=CONTRACT_MATRIX[source]['transition_probe']['mcu_registers']
    registers=CONTRACT_MATRIX[source]['transition_probe']['registers']
    expected_driver=CONTRACT_MATRIX[source].get('post_refresh_manual',{}).get('measurement_live_driver_bits',4 if 'refresh' in CONTRACT_MATRIX[source] else 0)
    frames=[];errors=[]
    for frame in range(8,12):
        for when in ('before','after'):
            selected=[r for r in rows if r['phase']=='5' and r['frame']==str(frame) and r['when']==when
                      and ((r['space']=='mcu' and r['addr'] in mcu) or (r['space']=='reg' and r['addr'] in registers))]
            v,issues=group(selected,mcu,registers)
            if not issues:
                if v['a102']&5!=expected_driver:issues.append('unexpected_live_AE_AWB_mode')
                if 'a103' in mcu and v['a103']!=0:issues.append('sequencer_command_pending')
                if not v['a355']&32:issues.append('force_unity_not_set')
                if v['a104'] not in (3,7):issues.append('sequencer_not_in_preview_or_capture')
                if any(v[a]!=128 for a in IFP):issues.append('physical_IFP_not_unity')
                if 'post_refresh_manual' in CONTRACT_MATRIX[source] and v['3012']!=4:issues.append('manual_coarse_not_4')
            frames.append(dict(frame=frame,when=when,values=v,errors=issues))
            errors.extend(f'{frame}:{when}:{x}' for x in issues)
    states={f['values']['a104'] for f in frames if f['values']['a104'] is not None}
    if len(states)>1:errors.append('sequencer_state_changed_during_measurement')
    return frames,errors


def analyse(directory):
    p=Path(directory)
    receipts=json.loads((p/'received-ram-sha256.json').read_text())
    names=[e['name'] for e in receipts]
    if len(set(names))!=len(names) or not {'manifest.txt','reads.csv','frames.csv','calls.csv'}.issubset(names):
        raise ValueError('missing or duplicate original receipt')
    for e in receipts:
        name=e['name']
        if Path(name).name!=name:raise ValueError('invalid receipt name')
        b=(p/name).read_bytes()
        if len(b)!=e['bytes'] or hashlib.sha256(b).hexdigest()!=e['sha256'] or f'{zlib.crc32(b):08x}'!=e['crc32']:
            raise ValueError('original evidence integrity failure: '+name)
    contract=manifest_records(p,'SWEEP_CONTRACT')
    if len(contract)!=1:raise ValueError('unregistered diagnostic source')
    source=contract[0].get('source')
    if source not in CONTRACT_MATRIX['exposure-isp-diagnostic-report/v3']['input_sources']:
        raise ValueError('unregistered diagnostic source')
    mcu=CONTRACT_MATRIX[source]['transition_probe']['mcu_registers']
    registers=CONTRACT_MATRIX[source]['transition_probe']['registers']
    expected=dict(source=source,states='1',points='1',stages='5',sensor='1',fps='5',
                  settle='8',measured='4',retry='none',ipc_dump='0',raw_x='256',raw_y='192',raw_w='128',raw_h='96')
    if contract!=[expected]:raise ValueError('unregistered diagnostic source')
    rows=read_csv(p/'reads.csv');frames,errors=measured(rows,source)
    captures=[r for r in read_csv(p/'frames.csv') if r['phase']=='5' and r['measured']=='1']
    if len(captures)!=4 or {r['frame'] for r in captures}!={'8','9','10','11'} or any(
            r['sensor']!='1' or r['label']!='sweep_s13_c0004' or
            r['ok']!='1' or r['received']!='1' or r['result']!='00000000' for r in captures):
        errors.append('missing_or_failed_measured_capture')
    transitions=[]
    for frame in range(8):
        for boundary in BOUNDARIES:
            chosen=[r for r in rows if r['phase']=='5' and r['frame']==str(frame) and r['when']==boundary
                    and r['name'] in ('isp_transition','isp_mcu_transition','exposure_transition')]
            values,issues=group(chosen,mcu,registers)
            transitions.append(dict(frame=frame,boundary=boundary,values=values,errors=issues,
                                    times_ms=[int(r['time_ms']) for r in chosen]))
            errors.extend(f'transition:{frame}:{boundary}:{x}' for x in issues)
    for label in ('SEQ_RESTORE','AWB_RESTORE'):
        records=manifest_records(p,label)
        mask=CONTRACT_MATRIX[source]['awb_force_unity'].get('restore_verify_mask',65535) if label=='AWB_RESTORE' else 65535
        if len(records)!=1 or records[0].get('verified')!='1' or (
                int(records[0].get('saved','-1'))&mask)!=(int(records[0].get('actual','-2'))&mask):
            errors.append(label+':not_verified')
    if 'refresh' in CONTRACT_MATRIX[source]:
        records=manifest_records(p,'PREVIEW_AWB_RESTORE')
        if len(records)!=1 or records[0].get('verified')!='1' or records[0].get('saved')!=records[0].get('actual'):
            errors.append('preview_AWB_not_restored')
        refresh=manifest_records(p,'SEQ_REFRESH')
        if len(refresh)!=2 or [r.get('restoring') for r in refresh]!=['0','1'] or any(
                r.get('verified')!='1' or r.get('command')!='5' or r.get('state') not in ('3','7') or
                not 2<=int(r.get('polls','0'))<=100 for r in refresh):
            errors.append('refresh_not_verified')
        commands=[r for r in read_csv(p/'calls.csv') if r['operation']=='WriteMcuVariableI2c' and r['addr']=='a103']
        if len(commands)!=2 or [r['label'] for r in commands]!=['sweep_refresh_apply','sweep_refresh_restore'] or any(
                r['sensor']!='1' or r['value']!='5' or r['result']!='00000000' for r in commands):
            errors.append('refresh_command_boundary_not_verified')
    if 'post_refresh_manual' in CONTRACT_MATRIX[source]:
        gate=manifest_records(p,'POST_REFRESH_MANUAL')
        if len(gate)!=1 or any(gate[0].get(k)!=v for k,v in dict(
                preview_awb='0',awb_r='128',awb_g='128',awb_b='128',command='0',verified='1').items()) or gate[0].get('state') not in ('3','7') or int(gate[0].get('seq','5'))&5:
            errors.append('manual_driver_disable_not_verified')
        final=manifest_records(p,'MANUAL_FINAL')
        if final!=[dict(phase='5',coarse='4',isp='128',verified='1')]:errors.append('final_manual_writes_not_verified')
        calls=read_csv(p/'calls.csv')
        requirements=[('sweep_refresh_apply','WriteMcuVariableI2c','a103','5'),
                      ('sweep_manual_preview_off','WriteMcuVariableI2c','a11f','0'),
                      ('sweep_manual_drivers_off','WriteMcuVariableI2c','a102',None),
                      ('sweep_s13_c0004','WriteRegisterI2c','3012','4')]
        requirements += [('sweep_final_isp','WriteRegisterI2c',a,'128') for a in IFP]
        order=[]
        for label,op,addr,value in requirements:
            matches=[i for i,r in enumerate(calls) if r['label']==label and r['operation']==op and r['addr']==addr and (value is None or r['value']==value) and r['result']=='00000000' and r['sensor']=='1']
            if len(matches)!=1:errors.append('manual_write_order_missing_or_duplicate:'+label+':'+addr)
            else:order.append(matches[0])
        starts=[i for i,r in enumerate(calls) if r['phase']=='5' and r['label']=='sweep_s13_c0004' and r['operation']=='StartCapture']
        if not starts or order!=sorted(order) or (order and order[-1]>=min(starts)):
            errors.append('manual_writes_not_after_refresh_before_capture')
        point=[i for i,r in enumerate(calls) if r['phase']=='5' and r['label']=='sweep_s13_c0004']
        restores=[i for i,r in enumerate(calls) if r['label']=='sweep_refresh_restore' and r['operation']=='WriteMcuVariableI2c' and r['addr']=='a103']
        if not point or len(restores)!=1 or restores[0]<=max(point):errors.append('refresh_during_manual_measurement')
    end=manifest_records(p,'SWEEP_END')
    if end!=[dict(complete='1',stages='5',restored='1')]:errors.append('diagnostic_incomplete')
    finish=manifest_records(p,'END')
    if len(finish)!=1 or any(finish[0].get(k)!=v for k,v in
            dict(complete='1',aborted='0',io_error='0',failures='0').items()):
        errors.append('run_did_not_finish_cleanly')
    gain_model=[]
    for t in transitions:
        v=t['values']
        if not t['errors']:
            awb=[v[a] for a in ('a34e','a34f','a34f','a350')]
            actual=[v[a] for a in IFP]
            predicted=[int(x*v['221f']/128+.5) for x in awb]
            gain_model.append(dict(frame=t['frame'],boundary=t['boundary'],ae_dgain=v['221f'],
                predicted_from_221f=predicted,actual=actual,matches=predicted==actual))
    return dict(schema='exposure-isp-diagnostic-report/v3',source=source,passed=not errors,
                calibration_validated=False,atomic_snapshot=False,files_verified=len(receipts),
                capture_id=hashlib.sha256((p/'manifest.txt').read_bytes()).hexdigest(),
                errors=errors,measured=frames,transitions=transitions,gain_model=gain_model)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path);parser.add_argument('--out',type=Path,required=True)
    a=parser.parse_args();report=analyse(a.directory)
    with a.out.open('x') as f:json.dump(report,f,indent=2);f.write('\n')
    print(json.dumps(dict(passed=report['passed'],files=report['files_verified'],errors=len(report['errors']),report=str(a.out))))
    raise SystemExit(0 if report['passed'] else 2)
