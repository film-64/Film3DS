#!/usr/bin/env python3
"""Verify the finite adaptive matrix; observed overwrite is not a failed RPC."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import zlib
from decode_exposure_state import read_csv
from exposure_session import manifest_records
from exposure_sweep_contract import CONTRACT_MATRIX, GAIN_MATRIX_SOURCE, GAIN_MATRIX_NODES, BOOT_STAGES

RULE=CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]
IFP=['32d4','32d6','32d8','32da']
CORE=['3012','3056','3058','305a','305c']

def analyse(directory):
    p=Path(directory);receipts=json.loads((p/'received-ram-sha256.json').read_text());names=[e['name'] for e in receipts]
    required={'manifest.txt','reads.csv','calls.csv','frames.csv','matrix_events.csv','matrix_plan.csv','startup_journal.txt'}
    if len(set(names))!=len(names) or not required.issubset(names):raise ValueError('missing or duplicate original receipt')
    for e in receipts:
        if Path(e['name']).name!=e['name']:raise ValueError('invalid receipt path')
        b=(p/e['name']).read_bytes()
        if len(b)!=e['bytes'] or hashlib.sha256(b).hexdigest()!=e['sha256'] or f'{zlib.crc32(b):08x}'!=e['crc32']:raise ValueError('original integrity failure:'+e['name'])
    records=lambda tag:manifest_records(p,tag)
    if records('MATRIX_CONTRACT')!=[dict(source=GAIN_MATRIX_SOURCE,nodes='8',targets='128,96,160',cycles='8_or_2',calibration='0')]:raise ValueError('unregistered matrix source')
    reads=read_csv(p/'reads.csv');calls=read_csv(p/'calls.csv');frames=read_csv(p/'frames.csv');events=read_csv(p/'matrix_events.csv');plan=read_csv(p/'matrix_plan.csv')
    errors=[];nodes=[]
    matrix_images=[name for name in names if name.endswith('.yuyv') and '-gm_' in name]
    if len(matrix_images)!=8 or any(name.endswith('.yuyv') and '-sweep_ref_00.' not in name and '-gm_' not in name for name in names):errors.append('unexpected_ROI_count_or_source')
    saved_rows=[r for r in reads if r['label']=='isp_update_save' and r['when']=='saved'];saved={r['addr']:int(r['value']) for r in saved_rows}
    if len(saved_rows)!=3 or set(saved)!={'a102','a355','a11f'} or any(r['result']!='00000000' or r['sensor']!='1' for r in saved_rows):errors.append('saved_MCU_missing')
    for tag,count in [('MATRIX_NODE',8),('MATRIX_NODE_END',8),('MATRIX_PERIOD',8),('MATRIX_SNAPSHOT',16),('MATRIX_TRIAL',24),('MATRIX_TRIAL_END',24),('CCM_WORD',176),('CCM_ALIAS_WINDOW',16),('MATRIX_ALIAS_TRANSPORT',16)]:
        if len(records(tag))!=count:errors.append('record_count:'+tag)
    if len(plan)!=24:errors.append('plan_count')
    if any(r.get('node') not in {str(i) for i in range(8)} for r in events+plan):errors.append('unknown_node')
    if [r.get('index') for r in records('MATRIX_NODE')]!=[str(i) for i in range(8)]:errors.append('node_order')
    if any(int(r['phase'])>12 for r in reads+calls+frames):errors.append('unknown_phase')
    def state(node,label,frame,when,target,baseline,bad):
        rs=[r for r in reads if r['phase']==str(node['index']+5) and r['label']==label and r['frame']==str(frame) and r['when']==when and r['name']=='gain_matrix_state']
        values={r['addr']:int(r['value']) for r in rs}
        seq=(saved.get('a102',0)&~15)|node['gate']
        fixed={'a102':seq,'a103':0,'a104':3,'a34e':128,'a34f':128,'a350':128,'3012':node['coarse'],'300a':2114 if node['fps']==5 else 704,**{a:528 for a in CORE[1:]}}
        valid=len(rs)==len(RULE['state_registers']) and set(values)==set(RULE['state_registers']) and all(r['sensor']=='1' and r['result']=='00000000' and r['space']==('mcu' if r['addr'].startswith('a') or r['addr']=='221f' else 'reg') for r in rs)
        valid=valid and all(values.get(a)==v for a,v in fixed.items()) and values.get('a355',0)&96==32
        valid=valid and all(1<=values.get(a,0)<=512 for a in IFP+['221f']) and values.get('300c',0)>0
        if baseline:valid=valid and values.get('221f')==baseline.get('221f') and values.get('300c')==baseline.get('300c')
        if not valid:bad.append(f'state:{label}:{frame}:{when}')
        times=[int(r['time_ms']) for r in rs]
        if times!=sorted(times):bad.append('state_time_order')
        return dict(values=values,held=valid and all(values.get(a)==target for a in IFP),times_ms=times)
    for node in GAIN_MATRIX_NODES:
        i=node['index'];phase=i+5;label=f"gm_n{i:02d}_f{node['fps']:02d}_d{node['gate']}_c{node['coarse']:04d}";bad=[]
        header=dict(index=str(i),phase=str(phase),fps=str(node['fps']),gate=str(node['gate']),coarse=str(node['coarse']),label=label)
        if header not in records('MATRIX_NODE'):bad.append('node_header')
        selected=[r for r in calls if r['phase']==str(phase)]
        prepared=state(node,label,-1,'prepared',128,None,bad);baseline=prepared['values']
        image_state=state(node,label,-1,'after_initial_image',0,baseline,bad)
        after_period=state(node,label,-1,'after_period',0,baseline,bad)
        def writes_ok(write_label,addresses,targets):
            wr=[r for r in selected if r['label']==write_label and r['operation']=='WriteRegisterI2c']
            if [r['addr'] for r in wr]!=addresses or [int(r['value']) for r in wr]!=targets or any(r['result']!='00000000' for r in wr):bad.append('setup_write:'+write_label)
        writes_ok('matrix_controls',CORE,[node['coarse']]+[528]*4);writes_ok('matrix_initial',IFP,[128]*4)
        writes_ok('isp_update_initial',IFP,[128]*4)
        for initial_label in ('isp_update_initial','matrix_initial'):
            ir=[r for r in reads if r['phase']==str(phase) and r['label']==initial_label and r['name']=='matrix_initial_gain']
            if [r['addr'] for r in ir]!=IFP or any(r['when']!='initial_observation' or r['space']!='reg' or r['sensor']!='1' or r['result']!='00000000' or not 1<=int(r['value'])<=512 for r in ir):bad.append('initial_IFP_observation:'+initial_label)

        gate=[r for r in selected if r['label']=='matrix_gate' and r['operation']=='WriteMcuVariableI2c']
        if len(gate)!=1 or gate[0]['addr']!='a102' or int(gate[0]['value'])!=((saved.get('a102',0)&~15)|node['gate']) or gate[0]['result']!='00000000':bad.append('gate_write')
        # Two complete byte-derived pipeline/Mode snapshots, never scalar error substitution.
        snapshots={}
        for when in ('before_stream','after_stop'):
            rs=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['when']==when and r['name']=='hg_snapshot']
            expected={('mcu',a) for a in RULE['snapshot']['mcu']}|{('reg',a) for a in RULE['snapshot']['physical']}
            if len(rs)!=len(expected) or {(r['space'],r['addr']) for r in rs}!=expected or any(r['sensor']!='1' or r['result']!='00000000' or (r['space']=='mcu' and int(r['addr'],16)&0x8000 and not 0<=int(r['value'])<=255) for r in rs):bad.append('snapshot:'+when)
            values={r['addr']:int(r['value']) for r in rs}
            br=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['when']==when and r['name']=='ccm_byte']
            aliases={f'{(int(a,16)|0x8000)+o:04x}' for a in RULE['snapshot']['derived_words'] for o in (0,1)}
            valid=len(br)==22 and {r['addr'] for r in br}==aliases and all(r['space']=='mcu' and r['sensor']=='1' and r['result']=='00000000' and 0<=int(r['value'])<=255 for r in br)
            raw={r['addr']:int(r['value']) for r in br};words=[r for r in records('CCM_WORD') if r.get('phase')==str(phase) and r.get('when')==when]
            valid=valid and len(words)==11 and {r.get('addr') for r in words}==set(RULE['snapshot']['derived_words'])
            if valid:
                for word in words:
                    a=word['addr'];hi=raw[f'{int(a,16)|0x8000:04x}'];lo=raw[f'{(int(a,16)|0x8000)+1:04x}'];v=(hi<<8)|lo
                    valid=valid and word.get('msb')==str(hi) and word.get('lsb')==str(lo) and word.get('value')==str(v);values[a]=v
            if not valid:bad.append('byte_snapshot:'+when)
            refs=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['when']==when and r['name']=='ccm_native_reference']
            after_refs=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['when']==when and r['name']=='ccm_native_after']
            windows=[r for r in records('CCM_ALIAS_WINDOW') if r.get('phase')==str(phase) and r.get('when')==when]
            if len(refs)!=1 or len(after_refs)!=1 or refs[0]['addr']!='2332' or after_refs[0]['addr']!='2332' or any(r['result']!='00000000' or r['sensor']!='1' or r['space']!='mcu' for r in refs+after_refs) or not valid:bad.append('native_reference:'+when)
            else:
                before=int(refs[0]['value']);after=int(after_refs[0]['value']);combined=values['2332']
                expected=dict(phase=str(phase),when=when,addr='2332',before=str(before),combined=str(combined),after=str(after),match_before=str(int(before==combined)),match_after=str(int(after==combined)))
                if windows!=[expected] or int(refs[0]['time_ms'])>int(br[0]['time_ms']) or int(after_refs[0]['time_ms'])<int(br[1]['time_ms']):bad.append('CCM_reference_window:'+when)
                values['ccm_snapshot_coherence']=dict(**expected,atomic=False)
            ar=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['when']==when and r['name'].startswith('matrix_alias_')]
            alias_names=['matrix_alias_native_before','matrix_alias_hi','matrix_alias_lo','matrix_alias_native_after']
            if [r['name'] for r in ar]!=alias_names or [r['addr'] for r in ar]!=['2703','a703','a704','2703'] or [r['value'] for r in ar]!=['640','2','128','640'] or any(r['result']!='00000000' or r['sensor']!='1' or r['space']!='mcu' for r in ar):bad.append('static_alias_transport:'+when)
            transport=[r for r in records('MATRIX_ALIAS_TRANSPORT') if r.get('phase')==str(phase) and r.get('when')==when]
            if transport!=[dict(phase=str(phase),when=when,before='640',combined='640',after='640',verified='1')]:bad.append('static_alias_marker:'+when)
            mr=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['when']==when and r['name']=='matrix_mode']
            if len(mr)!=19 or {r['addr'] for r in mr}!=set(RULE['snapshot']['Mode_readonly']) or any(r['space']!='mcu' or r['sensor']!='1' or r['result']!='00000000' for r in mr):bad.append('mode_snapshot:'+when)
            snapshots[when]=values
        ev=[r for r in events if r['node']==str(i)];period_rows=[r for r in ev if r['trial']=='-1'];pr=[r for r in records('MATRIX_PERIOD') if r.get('node')==str(i)];period=0
        if len(period_rows)==3 and len(pr)==1:
            if any(r['phase']!=str(phase) or r['fps']!=str(node['fps']) or r['target']!='0' or r['cycles']!='3' or r['period']!='0' or r['write_begin_ms']!='0' or r['write_end_ms']!='0' or r['held']!='0' for r in period_rows):bad.append('period_identity')
            steps=[int(r['latest'])-int(r['baseline']) for r in period_rows];period=int(statistics.median(steps));span=int(period_rows[-1]['returned_ms'])-int(period_rows[0]['returned_ms']);nominal=2000//node['fps']
            if [r['cycle'] for r in period_rows]!=['0','1','2'] or period<=0 or min(steps)*5<period*4 or max(steps)*5>period*6 or pr[0]!=dict(node=str(i),raw=str(period),host_span_ms=str(span),verified='1') or not nominal*4<=span*5<=nominal*6:bad.append('period_proof')
        else:bad.append('period_missing')
        trials=[];first_held=False;expected_cycles=[]
        for trial,target in enumerate(RULE['trial_targets']):
            cycles=8 if trial==0 or first_held else 2;expected_cycles.append(cycles);tl=f'{label}_t{target:03d}'
            why='mandatory' if trial==0 else 'first_held' if first_held else 'first_overwritten'
            if [r for r in plan if r['node']==str(i) and r['trial']==str(trial)]!=[dict(node=str(i),trial=str(trial),target=str(target),cycles=str(cycles),reason=why)]:bad.append('adaptive_plan:'+str(trial))
            if dict(node=str(i),trial=str(trial),target=str(target),cycles=str(cycles),label=tl) not in records('MATRIX_TRIAL'):bad.append('trial_header')
            rs=[r for r in reads if r['phase']==str(phase) and r['label']==tl and r['name']=='matrix_rewrite']
            if [r['addr'] for r in rs]!=IFP or any(r['when']!='rewrite_readback' or r['result']!='00000000' or r['sensor']!='1' or r['space']!='reg' or not 1<=int(r['value'])<=512 for r in rs):bad.append('rewrite_readback')
            wr=[r for r in selected if r['label']==tl and r['operation']=='WriteRegisterI2c']
            if [r['addr'] for r in wr]!=IFP or any(r['value']!=str(target) or r['result']!='00000000' for r in wr):bad.append('trial_writes')
            te=[r for r in ev if r['trial']==str(trial)];held=len(rs)==4 and all(r['value']==str(target) for r in rs);states=[];previous=0
            if len(te)!=cycles or [r['cycle'] for r in te]!=[str(c) for c in range(cycles)]:bad.append('cycle_count')
            for cycle,e in enumerate(te):
                g=state(node,tl,cycle,'vsync',target,baseline,bad);states.append(g);held=held and g['held']
                base=int(e['baseline']);latest=int(e['latest']);returned=int(e['returned_ms']);lo=int(e['write_begin_ms']);hi=int(e['write_end_ms'])
                valid=base>0 and latest>base and period>0 and period*4<=(latest-base)*5<=period*6 and (not previous or period*4<=(latest-previous)*5<=period*6)
                valid=valid and e['period']==str(period) and e['cycles']==str(cycles) and e['fps']==str(node['fps']) and e['phase']==str(phase) and e['target']==str(target) and e['held']==str(int(g['held'])) and lo<=hi<=returned
                if g['times_ms']:valid=valid and min(g['times_ms'])>=returned
                valid=valid and all(lo<=int(r['time_ms'])<=hi for r in wr+rs)
                if not valid:bad.append('consecutive_cycle:'+str(trial)+':'+str(cycle))
                previous=latest
            if not trial:first_held=held
            endings=[r for r in records('MATRIX_TRIAL_END') if r.get('node')==str(i) and r.get('trial')==str(trial)]
            if endings!=[dict(node=str(i),trial=str(trial),complete='1',held=str(int(held)),image_source='none')]:bad.append('trial_end')
            if te:
                first_time=min(int(r['time_ms']) for r in wr) if wr else 0;last_time=max(states[-1]['times_ms']) if states and states[-1]['times_ms'] else first_time
                if any(first_time<=int(r['time_ms'])<last_time and r['operation'] in ('SetReceiving','StartCapture','StopCapture','WriteMcuVariableI2c') for r in selected):bad.append('unexpected_work_in_no_receive_window')
            trials.append(dict(trial=trial,write_target=target,cycles=cycles,observed_held=held,eight_cycle_hold=held and cycles==8,states=states))
        raw=p/f's1-p{phase:03d}-{label}.yuyv';rr=[r for r in records('SWEEP_RAW') if r.get('phase')==str(phase)]
        if raw.name not in names or not raw.is_file() or raw.stat().st_size!=24576 or len(rr)!=1 or rr[0].get('crc32')!=f'{zlib.crc32(raw.read_bytes()):08x}':bad.append('initial_ROI_proof')
        state(node,label,3,'after_stop',0,baseline,bad)
        window_start=next((j for j,r in enumerate(calls) if r['phase']==str(phase) and r['label']==label and r['operation']=='GetVsyncInterruptEvent'),None)
        window_end=next((j for j,r in enumerate(calls) if r['phase']==str(phase) and r['label']==label and r['operation']=='CloseVsyncEvent'),None)
        active=calls[window_start:window_end+1] if window_start is not None and window_end is not None else []
        count=3+sum(expected_cycles)
        expected_counts={'SetReceiving':1,'StartCapture':1,'StopCapture':1,'WaitFrame':1,'IsFinishedReceiving':1,'CloseDMAEvent':1,'GetVsyncInterruptEvent':1,'CloseVsyncEvent':1,'ClearVsyncEvent':count,'WaitVsync':count,'GetLatestVsyncTiming':count*2}
        for op,n in expected_counts.items():
            chosen=[r for r in active if r['operation']==op]
            if len(chosen)!=n or any(r['sensor']!='1' or r['result']!='00000000' for r in chosen):bad.append('call_count:'+op)
        permitted=set(expected_counts)|{'FlushDataCache','InvalidateDataCache','ReadRegisterI2cExclusive','ReadMcuVariableI2cExclusive','WriteRegisterI2c','IsBusyAfterStop','ClearBuffer'}
        stop_rows=[(j,r) for j,r in enumerate(active) if r['operation']=='StopCapture']
        timing_rows=[j for j,r in enumerate(active) if r['operation']=='GetLatestVsyncTiming']
        node_state_times=[int(r['time_ms']) for r in reads if r['phase']==str(phase) and r['name']=='gain_matrix_state' and r['when']=='vsync']
        if len(stop_rows)!=1 or not timing_rows or stop_rows[0][0]<=max(timing_rows) or (node_state_times and int(stop_rows[0][1]['time_ms'])<max(node_state_times)):bad.append('stop_order')
        if any(r['operation'] not in permitted or r['result']!='00000000' for r in active):bad.append('unexpected_or_failed_window_call')
        if any(r['value']!='1' for r in active if r['operation']=='IsFinishedReceiving'):bad.append('pending_DMA')
        if any(r['operation']=='WriteRegisterI2c' and (r['label'] not in [f'{label}_t{t:03d}' for t in RULE['trial_targets']] or r['addr'] not in IFP) for r in active):bad.append('unexpected_window_write')
        vs=[r for r in active if r['operation']=='GetVsyncInterruptEvent'];vc=[r for r in active if r['operation']=='CloseVsyncEvent']
        if len(vs)!=1 or len(vc)!=1 or vs[0]['value']=='0' or vs[0]['value']!=vc[0]['value'] or any(r['value']!=vs[0]['value'] for r in active if r['operation'] in ('ClearVsyncEvent','WaitVsync')):bad.append('VSYNC_handle')
        for lab in [label]:
            rr=[r for r in active if r['label']==lab and r['operation']=='WaitFrame'];cc=[r for r in active if r['label']==lab and r['operation']=='CloseDMAEvent']
            if len(rr)!=1 or len(cc)!=1 or rr[0]['value']!=cc[0]['value'] or int(rr[0]['time_ms'])>int(cc[0]['time_ms']):bad.append('DMA_handle')
        fs=[r for r in frames if r['phase']==str(phase)]
        if len(fs)!=1 or fs[0]['label']!=label or fs[0]['frame']!='-1' or any(r['sensor']!='1' or r['measured']!='0' or r['ok']!='1' or r['received']!='1' or r['result']!='00000000' for r in fs):bad.append('DMA_frames')
        initial_wait=[r for r in active if r['operation']=='WaitFrame']
        if len(initial_wait)!=1 or not image_state['times_ms'] or min(image_state['times_ms'])<int(initial_wait[0]['time_ms']) or (period_rows and max(image_state['times_ms'])>int(period_rows[0]['returned_ms'])):bad.append('initial_image_order')
        if dict(index=str(i),complete='1',first_held=str(int(first_held))) not in records('MATRIX_NODE_END'):bad.append('node_end')
        if len(ev)!=3+sum(expected_cycles):bad.append('event_count')
        errors.extend(f'node{i}:{e}' for e in bad);nodes.append(dict(**node,phase=phase,sequencer_target=(saved.get('a102',0)&~15)|node['gate'],AE_dgain_baseline=baseline.get('221f'),period_raw=period,first_held=first_held,trials=trials,snapshots=snapshots,initial_image=dict(raw_file=raw.name,state=image_state,source='initial_node_DMA_before_trial_writes'),errors=bad))
    for r in calls:
        if r['operation']=='WriteMcuVariableI2c' and r['addr'] not in ('a102','a103','a11f','a355'):errors.append('unknown_MCU_write')
        if r['operation']=='WriteRegisterI2c' and r['addr'] not in CORE+IFP:errors.append('unknown_register_write')
    for a,n in [('a102',25),('a103',9),('a11f',17),('a355',3)]:
        rs=[r for r in calls if r['operation']=='WriteMcuVariableI2c' and r['addr']==a]
        if len(rs)!=n or any(r['result']!='00000000' for r in rs):errors.append('MCU_budget:'+a)
    profile=[r for r in calls if r['operation']=='SetFrameRate' and r['label'] in ('cal_setup','matrix_profile15','matrix_profile_restore5')]
    if [r['value'] for r in profile]!=['5','0','5'] or any(r['result']!='00000000' for r in profile):errors.append('profile_sequence')
    if any(r['value']!='5' for r in calls if r['operation']=='WriteMcuVariableI2c' and r['addr']=='a103'):errors.append('unknown_sequence_command')
    if len(records('MATRIX_PROFILE'))!=2 or [r.get('fps') for r in records('MATRIX_PROFILE')]!=['15','5'] or any(r.get('verified')!='1' for r in records('MATRIX_PROFILE')):errors.append('profile_completion')
    if len(records('SEQ_REFRESH'))!=9 or [r.get('restoring') for r in records('SEQ_REFRESH')]!=['0']*8+['1'] or any(r.get('verified')!='1' for r in records('SEQ_REFRESH')):errors.append('Refresh_completion')
    for tag in ('PREVIEW_AWB_RESTORE','SEQ_RESTORE','AWB_RESTORE'):
        rs=records(tag);mask=96 if tag=='AWB_RESTORE' else 65535
        if len(rs)!=1 or rs[0].get('verified')!='1' or (int(rs[0].get('saved','-1'))&mask)!=(int(rs[0].get('actual','-2'))&mask):errors.append('restore:'+tag)
    for original,restored,name in [('sweep_original','sweep_restore_controls','calibration_saved'),('sweep_ref_00','sweep_restore_isp','isp_gain')]:
        if name=='calibration_saved':a=[r for r in reads if r['label']==original and r['name']==name];z=[r for r in reads if r['label']==restored and r['name']==name];count=5
        else:a=[r for r in reads if r['label']==original and r['when']=='isp_pin'];z=[r for r in reads if r['label']==restored and r['when']=='verified'];count=4
        if len(a)!=count or len(z)!=count or any(r['result']!='00000000' for r in a+z) or {r['addr']:r['value'] for r in a}!={r['addr']:r['value'] for r in z}:errors.append('physical_restore:'+restored)
    startup=[];pattern=re.compile(r'BOOT run=(\d+) seq=(\d+) stage=(\d+) addr=([0-9a-f]{8}) result=([0-9a-f]{8}) at=(\d+) heap=(\d+) linear=(\d+)')
    for line in (p/'startup_journal.txt').read_text().splitlines():
        m=pattern.fullmatch(line)
        if not m:errors.append('boot_format');continue
        run,seq,stage,addr,result,at,heap,linear=m.groups();startup.append(dict(run=int(run),seq=int(seq),stage=int(stage),address=int(addr,16),result=result,at=int(at),heap=int(heap),linear=int(linear)))
    stages=[r['stage'] for r in startup]
    if [r['at'] for r in startup]!=sorted(r['at'] for r in startup) or any(r['heap']<=0 or r['linear']<=0 for r in startup):errors.append('boot_clock_or_heap')
    if len(startup)!=768 or [r['seq'] for r in startup]!=list(range(len(startup))) or len({r['run'] for r in startup})!=1 or stages[:12]!=list(range(1,13)) or stages[-4:]!=[19,20,21,22] or any(r['stage'] not in BOOT_STAGES or r['stage'] in (25,255) or r['result']!='00000000' for r in startup):errors.append('boot_lifecycle')
    for stage,n in ((13,8),(14,8),(15,16),(16,352),(17,352),(18,16)):
        if stages.count(stage)!=n:errors.append('boot_stage_count:'+str(stage))
    br=[r for r in reads if r['name']=='ccm_byte'];paired=[r for r in startup if r['stage'] in (16,17)]
    if len(paired)!=len(br)*2:errors.append('boot_byte_count')
    else:
        for i,r in enumerate(br):
            a,z=paired[2*i:2*i+2]
            if a['stage']!=16 or z['stage']!=17 or a['address']!=int(r['addr'],16) or z['address']!=a['address'] or not a['at']<=int(r['time_ms'])<=z['at']:errors.append('boot_byte_boundary')
    if records('MATRIX_END')!=[dict(complete='1',nodes='8',restored='1')]:errors.append('matrix_incomplete')
    end=records('END')
    if len(end)!=1 or any(end[0].get(k)!=v for k,v in dict(complete='1',aborted='0',io_error='0').items()):errors.append('run_incomplete')
    controllable=[n['index'] for n in nodes if not n['errors'] and all(t['eight_cycle_hold'] for t in n['trials'])]
    return dict(schema='camera-gain-matrix-report/v4',source=GAIN_MATRIX_SOURCE,passed=not errors,calibration_validated=False,files_verified=len(receipts),nodes=nodes,controllable_nodes=controllable if not errors else [],errors=errors,writer_identified=False,timing_cause_identified=False,startup_journal=startup)

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('directory',type=Path);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();r=analyse(a.directory)
    with a.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps(dict(passed=r['passed'],errors=r['errors'],controllable_nodes=r['controllable_nodes'],files=r['files_verified'])))
    raise SystemExit(0 if r['passed'] else 2)
