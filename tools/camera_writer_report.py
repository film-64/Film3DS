#!/usr/bin/env python3
"""Join one registered device window with complete Camera outbound IPC proof."""
import argparse
import hashlib
import json
from pathlib import Path
import zlib
from decode_exposure_state import read_csv
from exposure_session import manifest_records
from exposure_sweep_contract import WRITER_WINDOW_SOURCE,WRITER_OBSERVE_SOURCE,CONTRACT_MATRIX
from exposure_isp_update_report import state,IFP
from camera_writer_trace import fields,word,writes


def analyse(batch):
    batch=Path(batch);p=batch/'recovered';trace=batch/'trace';errors=[]
    receipts=json.loads((p/'received-ram-sha256.json').read_text());names=[e['name'] for e in receipts]
    if len(set(names))!=len(names):raise ValueError('duplicate receipt')
    for e in receipts:
        if Path(e['name']).name!=e['name']:raise ValueError('invalid receipt')
        b=(p/e['name']).read_bytes()
        if len(b)!=e['bytes'] or hashlib.sha256(b).hexdigest()!=e['sha256'] or f'{zlib.crc32(b):08x}'!=e['crc32']:raise ValueError('original integrity failure')
    contracts=manifest_records(p,'WRITER_WINDOW_CONTRACT')
    source=contracts[0].get('source') if len(contracts)==1 else None
    if source not in (WRITER_WINDOW_SOURCE,WRITER_OBSERVE_SOURCE):raise ValueError('unknown device source')
    observed=source==WRITER_OBSERVE_SOURCE
    expected=dict(source=source,arms='1',vsync='3',sensor='1',fps='5',retry='none')
    if manifest_records(p,'WRITER_WINDOW_CONTRACT')!=[expected]:raise ValueError('unknown device source')
    calls=read_csv(p/'calls.csv');rows=read_csv(p/'reads.csv');frames=read_csv(p/'frames.csv');label='no_receive_128'
    saved={r['addr']:int(r['value']) for r in rows if r['label']=='isp_update_save' and r['when']=='saved' and r['result']=='00000000'}
    if set(saved)!={'a102','a355','a11f'}:errors.append('missing_saved_controls')
    boundaries=[(-1,'initial'),(0,'after_dma'),(0,'after_rewrite'),(1,'post_vsync'),(2,'post_vsync'),(3,'post_vsync'),(3,'after_stop')]
    groups=[state(rows,5,label,f,b,saved) for f,b in boundaries]
    for g in groups:errors.extend(f"{g['frame']}:{g['boundary']}:{e}" for e in g['errors'])
    phase=[r for r in calls if r['phase']=='5'];point=[(i,r) for i,r in enumerate(calls) if r['phase']=='5' and r['label']==label]
    receives=[i for i,r in point if r['operation']=='SetReceiving'];stops=[i for i,r in point if r['operation']=='StopCapture']
    active=calls[receives[0]:stops[-1]+1] if receives and stops else []
    counts=dict(SetReceiving=1,StartCapture=1,StopCapture=1,WaitFrame=1,IsFinishedReceiving=1,CloseDMAEvent=1,ClearVsyncEvent=3,WaitVsync=3,GetLatestVsyncTiming=6)
    for op,count in counts.items():
        selected=[r for r in active if r['operation']==op]
        if len(selected)!=count or any(r['result']!='00000000' or r['sensor']!='1' for r in selected):errors.append('bad_window_call:'+op)
    allowed=set(counts)|{'WriteRegisterI2c'}
    if any(r['operation'] not in allowed for r in active):errors.append('unregistered_window_operation')
    if any(r['operation']=='IsFinishedReceiving' and r['value']!='1' for r in active):errors.append('pending_DMA_not_excluded')
    for op in ('SetReceiving','StartCapture'):
        if sum(r['operation']==op for r in phase)!=1:errors.append('extra_capture_operation')
    sequence=[r['operation'] for r in active if r['operation'] in ('GetLatestVsyncTiming','ClearVsyncEvent','WaitVsync')]
    if sequence!=['GetLatestVsyncTiming','ClearVsyncEvent','WaitVsync','GetLatestVsyncTiming']*3:errors.append('VSYNC_order_invalid')
    def pos(op):return [i for i,r in enumerate(active) if r['operation']==op]
    changed=pos('WriteRegisterI2c');waits=pos('WaitFrame');closed=pos('CloseDMAEvent');finished=pos('IsFinishedReceiving');vs=pos('WaitVsync')
    if not (len(changed)==4 and len(waits)==len(closed)==len(finished)==1 and vs and waits[0]<finished[0]<closed[0]<min(changed)<=max(changed)<vs[0]):errors.append('DMA_close_rewrite_order_missing')
    elif active[waits[0]]['value']!=active[closed[0]]['value']:errors.append('wrong_DMA_handle')
    for name in ('isp_update_initial','no_receive_rewrite'):
        observational=observed and name=='no_receive_rewrite'
        proof=[r for r in rows if r['phase']=='5' and r['label']==name and r['when']==('observed' if observational else 'verified')]
        written=[r for r in phase if r['label']==name and r['operation']=='WriteRegisterI2c']
        if len(written)!=4 or {r['addr'] for r in written}!=set(IFP) or any(r['value']!='128' or r['result']!='00000000' or r['sensor']!='1' for r in written):errors.append('write_missing:'+name)
        if len(proof)!=4 or {r['addr'] for r in proof}!=set(IFP) or any(r['result']!='00000000' or r['sensor']!='1' or not (1<=int(r['value'])<=512 if observational else r['value']=='128') for r in proof):errors.append('write_readback_missing:'+name)
    if observed:
        rewrite=manifest_records(p,'WRITER_REWRITE')
        if len(rewrite)!=1 or rewrite[0].get('writes_ok')!='1':errors.append('instrumented_rewrite_missing')
        baseline=[r for r in rows if r['name']=='writer_unit_baseline']
        if len(baseline)!=4 or {r['addr'] for r in baseline}!=set(IFP) or any(r['sensor']!='1' or r['phase']!='5' or r['when']!='pre_GO' or r['value']!='128' or r['result']!='00000000' for r in baseline):errors.append('pre_GO_unity_control_missing')
    if any(r['operation']=='WriteRegisterI2c' and (r['label']!='no_receive_rewrite' or r['addr'] not in IFP) for r in active):errors.append('hidden_window_write')
    captures=[r for r in frames if r['phase']=='5']
    if len(captures)!=1 or any(r.get(k)!=v for r in captures for k,v in dict(sensor='1',label=label,frame='0',measured='0',ok='1',result='00000000',received='1').items()):errors.append('initial_DMA_incomplete')
    vsync=manifest_records(p,'NO_RECEIVE_VSYNC');timings=manifest_records(p,'NO_RECEIVE_TIMING');previous=0
    if len(vsync)!=3 or len(timings)!=6:errors.append('VSYNC_proof_missing')
    for index in range(1,4):
        selected=[r for r in vsync if r.get('observation')==str(index)]
        valid=len(selected)==1
        if valid:
            v=selected[0];base=int(v['baseline']);latest=int(v['latest'])
            valid=v.get('verified')=='1' and base>0 and latest>base and latest>previous;previous=latest
            proof=[r for r in timings if r.get('observation')==str(index)]
            valid=valid and proof==[dict(observation=str(index),when='baseline',raw=str(base),result='00000000'),dict(observation=str(index),when='latest',raw=str(latest),result='00000000')]
        if not valid:errors.append('stale_or_missing_VSYNC:'+str(index))
        red=[r for r in rows if r['name']=='no_receive_red' and r['frame']==str(index)]
        if len(red)!=8 or any(r['phase']!='5' or r['label']!=label or r['sensor']!='1' or r['addr']!='32d4' or r['result']!='00000000' for r in red):errors.append('red_reads_incomplete')
    gate=manifest_records(p,'WRITER_GATE')
    if gate!=[dict(state='listening',port='5001',phase='5'),dict(state='GO',phase='5'),dict(state='RELEASE',received='1',phase='5'),dict(state='closed',complete='1',phase='5')]:errors.append('gate_incomplete')
    if manifest_records(p,'WRITER_WINDOW_END')!=[dict(complete='1',restored='1')]:errors.append('window_or_restore_incomplete')
    if manifest_records(p,'NO_RECEIVE_WINDOW')!=[dict(complete='1',observations='3',raw_provenance='first_DMA_only')]:errors.append('no_receive_window_incomplete')
    raw=p/'s1-p005-no_receive_128.yuyv';raw_records=[r for r in manifest_records(p,'SWEEP_RAW') if r.get('phase')=='5']
    if raw.name not in names or not raw.is_file() or raw.stat().st_size!=24576 or len(raw_records)!=1 or raw_records[0].get('crc32')!=f'{zlib.crc32(raw.read_bytes()):08x}':errors.append('first_DMA_ROI_missing')
    original={r['addr']:r['value'] for r in rows if r['label']=='sweep_original' and r['name']=='calibration_saved' and r['result']=='00000000'}
    restored_controls={r['addr']:r['value'] for r in rows if r['label']=='sweep_restore_controls' and r['name']=='calibration_saved' and r['result']=='00000000'}
    if set(original)!={'3012','3056','3058','305a','305c'} or original!=restored_controls:errors.append('physical_controls_restore_missing')
    for marker in ('SEQ_RESTORE','AWB_RESTORE','PREVIEW_AWB_RESTORE'):
        restored=manifest_records(p,marker);mask=96 if marker=='AWB_RESTORE' else 65535
        if len(restored)!=1 or restored[0].get('verified')!='1' or (int(restored[0].get('actual','-1'))&mask)!=(int(restored[0].get('saved','-2'))&mask):errors.append('restore_missing:'+marker)
    refresh=manifest_records(p,'SEQ_REFRESH')
    if len(refresh)!=2 or [r.get('restoring') for r in refresh]!=['0','1'] or any(r.get('verified')!='1' for r in refresh):errors.append('refresh_proof_missing')
    end=manifest_records(p,'END')
    if len(end)!=1 or any(end[0].get(k)!=v for k,v in dict(complete='1',io_error='0',aborted='0').items()):errors.append('device_run_incomplete')
    status=json.loads((trace/'trace-status.json').read_text());events=[json.loads(line) for line in (trace/'trace.jsonl').read_text().splitlines()]
    if status.get('source')!=('camera-writer-trace/v2' if observed else 'camera-writer-trace/v1'):raise ValueError('unknown observer source')
    for key in ('complete','identity_matched','detached','catch_disabled','gate_released','passed'):
        if status.get(key) is not True:errors.append('trace_incomplete:'+key)
    text=(trace/'camera-text.bin').read_bytes()
    if len(text)!=86016 or hashlib.sha256(text).hexdigest()!=status.get('text_sha256'):errors.append('live_identity_bytes_invalid')
    expected_identity=json.loads((trace/'expected-camera-identity.json').read_text())
    if hashlib.sha256(text).hexdigest()!=expected_identity.get('text_sha256') or any(
            text[int(e['address'],16)-0x100000:int(e['address'],16)-0x100000+16].hex()!=e['bytes'] for e in expected_identity['entry_bytes'].values()):errors.append('expected_identity_mismatch')
    kinds=[r['kind'] for r in events]
    if any(kinds.count(k)!=1 for k in ('identity','gate_ready','gate_GO','gate_DONE','gate_RELEASE')):errors.append('trace_gate_evidence_missing')
    else:
        ready,identity,go,done,release=[kinds.index(k) for k in ('gate_ready','identity','gate_GO','gate_DONE','gate_RELEASE')]
        if not ready<identity<go<done<release:errors.append('trace_gate_order_invalid')
        if events[identity].get('i2c_handle')!=status.get('i2c_handle') or events[identity].get('sha256')!=status.get('text_sha256'):errors.append('trace_identity_disagrees')
        if any(not go<i<done for i,r in enumerate(events) if r['kind'] in ('i2c_entry','i2c_return')):errors.append('entry_outside_trace_window')
    entries=[r for r in events if r['kind']=='i2c_entry'];returns=[r for r in events if r['kind']=='i2c_return'];stops=[r for r in events if r['kind']=='stop']
    if len(entries)!=status.get('entries') or len(stops)!=status.get('stops'):errors.append('trace_count_mismatch')
    positive=set();IFP_writes=[];unknown=[]
    last_stop=None;used_stops=set()
    for index,event in enumerate(events):
        if event['kind']=='stop':last_stop=(index,fields(event['packet']))
        if event['kind'] not in ('i2c_entry','i2c_return'):continue
        if last_stop is None or last_stop[0] in used_stops:errors.append('missing_independent_stop_proof');continue
        used_stops.add(last_stop[0]);f=last_stop[1]
        if f['thread']!=event['thread']:errors.append('wrong_trace_thread')
        if event['kind']=='i2c_entry':
            if f.get('syscall_entry')!='32' or word(f['0'])!=status.get('i2c_handle') or event.get('handle')!=status.get('i2c_handle') or any(word(f[k])!=event[v] for k,v in [('f','pc'),('e','lr'),('d','sp')]):errors.append('fake_I2C_entry_proof')
        elif f.get('syscall_return')!='32' or word(f['0'])!=event.get('register_r0' if observed else 'transport_result'):errors.append('fake_I2C_return_proof')
    for entry in entries:
        paired=[r for r in returns if r['entry_id']==entry['entry_id'] and r['thread']==entry['thread']]
        if len(paired)!=1 or (not observed and paired[0].get('transport_result')!=0) or paired[0]['words'][1]!=0:errors.append('IPC_completion_missing_or_failed')
        decoded,covered=writes(entry)
        if decoded!=entry['writes'] or covered!=entry['covered']:errors.append('decoded_write_disagrees')
        if not covered:unknown.append(entry['words'][0]>>16)
        for w in decoded:
            if w['device']==2 and w['addr'] in (0x32d4,0x32d6,0x32d8,0x32da):
                if len(entry.get('stack_hex',''))!=256:errors.append('missing_IFP_caller_stack')
                IFP_writes.append(dict(entry_id=entry['entry_id'],pc=entry['pc'],lr=entry['lr'],**w))
                if w.get('value')==128:positive.add(w['addr'])
    if len(returns)!=len(entries) or len({r['entry_id'] for r in entries})!=len(entries):errors.append('IPC_pairing_incomplete')
    if positive!={0x32d4,0x32d6,0x32d8,0x32da}:errors.append('positive_IFP128_control_missing')
    if unknown:errors.append('unknown_I2C_command_no_negative_claim')
    final=[groups[-2]['values'].get(a) for a in IFP]
    external=[w for w in IFP_writes if w.get('value')!=128 or w.get('rmw')]
    conclusion='inconclusive' if errors else 'Camera_outbound_IFP_write_observed' if external else 'IFP_changed_without_Camera_outbound_IFP_write' if final!=[128]*4 else 'overwrite_not_reproduced'
    return dict(schema='camera-writer-report/v2' if observed else 'camera-writer-report/v1',source=source,passed=not errors,calibration_validated=False,
                conclusion=conclusion,errors=errors,files_verified=len(receipts),groups=groups,vsync=vsync,IFP_writes=IFP_writes,
                unknown_I2C_commands=unknown,positive_control_channels=sorted(positive),trace=status,
                scope='Camera_outbound_IPC_only',all_external_I2C_writers_excluded=False)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('batch',type=Path);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();r=analyse(args.batch)
    with args.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps({k:r[k] for k in ('passed','conclusion','errors','IFP_writes','trace')},ensure_ascii=False))
    raise SystemExit(0 if r['passed'] else 2)
