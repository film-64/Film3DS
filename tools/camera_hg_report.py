#!/usr/bin/env python3
"""Fail-closed local HG isolation evidence verifier; no calibration claim."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import zlib
from decode_exposure_state import read_csv
from exposure_session import manifest_records
from exposure_sweep_contract import CONTRACT_MATRIX, HG_SOURCE, HG_DELAYS, HG_MCU, HG_PHYSICAL, CCM_SOURCE, CCM_BYTES_SOURCE, CCM_BOOT_SOURCE, BOOT_STAGES
from exposure_isp_update_report import state, IFP


def analyse(directory,source=HG_SOURCE):
    if source not in (HG_SOURCE,CCM_SOURCE,CCM_BYTES_SOURCE,CCM_BOOT_SOURCE):raise ValueError('unregistered diagnostic report source')
    rule=CONTRACT_MATRIX[source];boot=source==CCM_BOOT_SOURCE;byte_words=source in (CCM_BYTES_SOURCE,CCM_BOOT_SOURCE);ccm=source in (CCM_SOURCE,CCM_BYTES_SOURCE,CCM_BOOT_SOURCE)
    mcu=rule['snapshot']['mcu'];physical=rule['snapshot']['physical']
    p=Path(directory); receipts=json.loads((p/'received-ram-sha256.json').read_text())
    names=[r['name'] for r in receipts]
    if len(set(names))!=len(names) or not {'manifest.txt','reads.csv','calls.csv','frames.csv'}.issubset(names):
        raise ValueError('missing or duplicate original receipt')
    for r in receipts:
        if Path(r['name']).name!=r['name']:raise ValueError('invalid receipt name')
        b=(p/r['name']).read_bytes()
        if len(b)!=r['bytes'] or hashlib.sha256(b).hexdigest()!=r['sha256'] or f'{zlib.crc32(b):08x}'!=r['crc32']:
            raise ValueError('original integrity failure: '+r['name'])
    if manifest_records(p,'HG_CONTRACT')!=[dict(source=source,arms='3',trials='4',fps='5',observer='none',retry='none')]:
        raise ValueError('unregistered HG source')
    reads=read_csv(p/'reads.csv');calls=read_csv(p/'calls.csv');frames=read_csv(p/'frames.csv')
    errors=[];arms=[];startup=[]
    if boot:
        path=p/'startup_journal.txt'
        if path.name not in names or not path.is_file() or not 1<=path.stat().st_size<=128*1024:errors.append('startup_journal_missing')
        else:
            pattern=re.compile(r'BOOT run=(\d+) seq=(\d+) stage=(\d+) addr=([0-9a-f]{8}) result=([0-9a-f]{8}) at=(\d+) heap=(\d+) linear=(\d+)')
            for line in path.read_text().splitlines():
                m=pattern.fullmatch(line)
                if not m:errors.append('startup_journal_format');continue
                run,seq,stage,addr,result,at,heap,linear=m.groups()
                startup.append(dict(run=int(run),seq=int(seq),stage=int(stage),address=int(addr,16),result=result,at=int(at),heap=int(heap),linear=int(linear)))
            if not 1<=len(startup)<=768 or [r['seq'] for r in startup]!=list(range(len(startup))) or len({r['run'] for r in startup})!=1 or any(r['stage'] not in BOOT_STAGES or r['result']!='00000000' or r['stage'] in (25,255) or not r['run'] or r['heap']<=0 or r['linear']<=0 for r in startup):errors.append('startup_journal_invalid_or_fault')
            if [r['at'] for r in startup]!=sorted(r['at'] for r in startup):errors.append('startup_journal_time_order')
            stages=[r['stage'] for r in startup]
            if stages[:12]!=list(range(1,13)) or stages[-4:]!=[19,20,21,22]:errors.append('startup_journal_lifecycle')
            for stage,count in ((13,3),(14,3),(15,6),(16,132),(17,132),(18,6)):
                if stages.count(stage)!=count:errors.append('startup_journal_count:'+str(stage))
            paired=[r for r in startup if r['stage'] in (16,17)]
            expected_reads=[r for r in reads if r['name']=='ccm_byte']
            if len(paired)!=2*len(expected_reads):errors.append('startup_byte_pairs')
            else:
                for i,r in enumerate(expected_reads):
                    a,z=paired[2*i:2*i+2]
                    if a['stage']!=16 or z['stage']!=17 or a['address']!=int(r['addr'],16) or z['address']!=a['address'] or not a['at']<=int(r['time_ms'])<=z['at']:errors.append('startup_byte_boundary')
    elif 'startup_journal.txt' in names:errors.append('unregistered_startup_journal')
    for name,count in (('HG_ARM',3),('HG_ARM_END',3),('HG_WINDOW',3),('HG_VSYNC',12),('HG_TRIAL',12)):
        if len(manifest_records(p,name))!=count:errors.append('unexpected_record_count:'+name)
    if any(int(r['phase'])>7 for r in calls+reads+frames):errors.append('unregistered_phase')
    if sum(r['name']=='hg_snapshot' for r in reads)!=6*(len(mcu)+len(physical)):errors.append('snapshot_global_count')
    if len(manifest_records(p,'CCM_WORD'))!=(66 if byte_words else 0):errors.append('derived_word_count')
    if len(manifest_records(p,'CCM_ALIAS_CHECK'))!=(6 if byte_words else 0):errors.append('alias_check_count')
    if sum(r['name']=='ccm_byte' for r in reads)!=(132 if byte_words else 0):errors.append('CCM_byte_count')
    if sum(r['name']=='ccm_native_reference' for r in reads)!=(6 if byte_words else 0):errors.append('CCM_reference_count')
    if sum(r['name']=='hg_rewrite' for r in reads)!=48:errors.append('rewrite_global_count')
    if len(manifest_records(p,'HG_POLL'))!=sum(r['name']=='hg_red' for r in reads):errors.append('poll_global_count')
    saved_rows=[r for r in reads if r['label']=='isp_update_save' and r['when']=='saved']
    saved={r['addr']:int(r['value']) for r in saved_rows}
    if len(saved_rows)!=3 or set(saved)!={'a102','a355','a11f'} or any(r['sensor']!='1' or r['space']!='mcu' or r['result']!='00000000' for r in saved_rows):
        errors.append('missing_saved_MCU');saved={}
    for index,enabled in enumerate(rule['arms']):
        phase=5+index;label=rule['labels'][index] if ccm else f'hg_{index}_{"on" if enabled else "off"}';bad=[]
        target=(saved.get('a102',0)&~13)|(8 if enabled else 0)
        header=dict(index=str(index),phase=str(phase),label=label,enabled=str(enabled),target=str(target),gate_verified='1')
        if header not in manifest_records(p,'HG_ARM'):bad.append('gate_header_missing')
        if dict(index=str(index),phase=str(phase),complete='1') not in manifest_records(p,'HG_ARM_END'):bad.append('arm_incomplete')
        selected=[r for r in calls if r['phase']==str(phase)]
        chosen=lambda op:[r for r in selected if r['operation']==op]
        # Exact setup writes are registered separately from observational trials.
        for setup_label,addresses,targets in (
            ('isp_update_initial',IFP,[128]*4),('hg_final_isp',IFP,[128]*4),
            ('hg_final_controls',['3012','3056','3058','305a','305c'],[4,528,528,528,528])):
            wr=[r for r in selected if r['label']==setup_label and r['operation']=='WriteRegisterI2c']
            rb=[r for r in reads if r['phase']==str(phase) and r['label']==setup_label and r['when']=='verified'] if setup_label!='hg_final_controls' else [r for r in reads if r['phase']==str(phase) and r['name']=='hg_final_control']
            for rs in (wr,rb):
                if [r['addr'] for r in rs]!=addresses or [int(r['value']) for r in rs]!=targets or any(r['result']!='00000000' or r['sensor']!='1' for r in rs):bad.append('setup_write_proof:'+setup_label)
        gates=[r for r in selected if r['label']=='hg_gate']
        if len(gates)!=1 or gates[0]['operation']!='WriteMcuVariableI2c' or gates[0]['addr']!='a102' or gates[0]['value']!=str(target) or gates[0]['result']!='00000000':bad.append('gate_write_not_verified')
        gate_reads=[r for r in reads if r['phase']==str(phase) and r['name']=='hg_gate']
        if len(gate_reads)!=1 or gate_reads[0]['value']!=str(target) or gate_reads[0]['result']!='00000000' or gate_reads[0]['addr']!='a102' or gate_reads[0]['space']!='mcu':bad.append('gate_readback_not_verified')
        if ccm:
            wr=[(i,r) for i,r in enumerate(selected) if r['operation']=='WriteMcuVariableI2c' and r['label']=='ccm_mode']
            rb=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['name']=='ccm_mode']
            expected_mode=((saved.get('a355',0)|32)&~64)|(64 if rule['normalization_disable'][index] else 0)
            cmd=[i for i,r in enumerate(selected) if r['operation']=='WriteMcuVariableI2c' and r['addr']=='a103' and r['label']=='sweep_refresh_apply']
            if len(wr)!=1 or len(cmd)!=1 or wr[0][0]>=cmd[0] or wr[0][1]['addr']!='a355' or wr[0][1]['value']!=str(expected_mode) or wr[0][1]['result']!='00000000':bad.append('CCM_mode_write_before_Refresh_missing')
            if len(rb)!=1 or rb[0]['addr']!='a355' or rb[0]['space']!='mcu' or rb[0]['when']!='before_refresh' or rb[0]['result']!='00000000' or rb[0]['sensor']!='1' or rb[0]['frame']!='-1' or int(rb[0]['value'])&96!=expected_mode&96:bad.append('CCM_mode_readback_missing')
            modes=[m for m in manifest_records(p,'CCM_MODE') if m.get('index')==str(index)]
            if len(modes)!=1 or modes[0].get('phase')!=str(phase) or modes[0].get('disabled')!=str(rule['normalization_disable'][index]) or modes[0].get('target')!=str(expected_mode) or modes[0].get('verified')!='1' or (int(modes[0].get('actual','0'))&96)!=(expected_mode&96):bad.append('CCM_mode_record_missing')
        snapshots={}
        for when in ('before_stream','after_stop'):
            rows=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['name']=='hg_snapshot' and r['when']==when]
            expected={('mcu',a) for a in mcu}|{('reg',a) for a in physical}
            if len(rows)!=len(expected) or {(r['space'],r['addr']) for r in rows}!=expected or any(r['sensor']!='1' or r['result']!='00000000' or r['frame']!='-1' or (r['space']=='mcu' and int(r['addr'],16)&0x8000 and not 0<=int(r['value'])<=255) for r in rows):bad.append('snapshot_missing_or_invalid:'+when)
            snapshots[when]={r['addr']:int(r['value']) for r in rows}
            if byte_words:
                byte_rows=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['when']==when and r['name']=='ccm_byte']
                word_records=[w for w in manifest_records(p,'CCM_WORD') if w.get('phase')==str(phase) and w.get('when')==when]
                aliases={f'{(int(a,16)|0x8000)+offset:04x}' for a in rule['snapshot']['derived_words'] for offset in (0,1)}
                valid=len(byte_rows)==22 and {r['addr'] for r in byte_rows}==aliases and len(word_records)==11 and {w.get('addr') for w in word_records}==set(rule['snapshot']['derived_words'])
                valid=valid and all(r['sensor']=='1' and r['space']=='mcu' and r['frame']=='-1' and r['result']=='00000000' and 0<=int(r['value'])<=255 for r in byte_rows)
                raw={r['addr']:int(r['value']) for r in byte_rows}
                if valid:
                    for w in word_records:
                        a=w['addr'];high=f'{int(a,16)|0x8000:04x}';low=f'{(int(a,16)|0x8000)+1:04x}'
                        combined=(raw[high]<<8)|raw[low]
                        if w.get('msb')!=str(raw[high]) or w.get('lsb')!=str(raw[low]) or w.get('value')!=str(combined):valid=False
                        snapshots[when][a]=combined
                if not valid:bad.append('CCM_byte_proof:'+when)
                ref=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['when']==when and r['name']=='ccm_native_reference']
                checks=[w for w in manifest_records(p,'CCM_ALIAS_CHECK') if w.get('phase')==str(phase) and w.get('when')==when]
                if len(ref)!=1 or len(checks)!=1 or ref[0]['addr']!='2332' or ref[0]['space']!='mcu' or ref[0]['frame']!='-1' or ref[0]['sensor']!='1' or ref[0]['result']!='00000000' or int(ref[0]['value'])!=snapshots[when].get('2332') or checks[0]!=dict(phase=str(phase),when=when,addr='2332',native=ref[0]['value'],combined=ref[0]['value'],verified='1'):bad.append('CCM_native_reference:'+when)
        boundaries=[(-1,'initial'),(-1,'gated'),(0,'after_dma')]+[(t,'after_poll') for t in range(4)]+[(4,'after_stop')]
        groups=[]
        for frame,when in boundaries:
            g=state(reads,phase,label,frame,when)
            expected_seq=(saved.get('a102',0)&~5) if when=='initial' else target
            if when!='initial':
                ae=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['frame']==str(frame) and r['when']==when and r['name']=='hg_fixed_ae']
                if len(ae)!=1 or ae[0]['space']!='mcu' or ae[0]['addr']!='221f' or ae[0]['value']!='128' or ae[0]['result']!='00000000':g['errors'].append('AE_dgain_not_fixed')
            if g['values'].get('221f')!=128:g['errors'].append('AE_dgain_state_changed')
            if g['values'].get('a102')!=expected_seq:g['errors'].append('HG_gate_or_other_seq_bit_drift')
            awb_target=(saved.get('a355',0)|32)
            if ccm:awb_target=(awb_target&~64)|(64 if rule['normalization_disable'][index] else 0)
            if saved and (g['values'].get('a355',0)&96)!=(awb_target&96):g['errors'].append('AWB_control_drift')
            bad.extend(f'{frame}:{when}:{e}' for e in g['errors']);groups.append(g)
        first=next((i for i,r in enumerate(calls) if r['phase']==str(phase) and r['operation']=='SetReceiving'),None)
        last=next((i for i,r in enumerate(calls) if r['phase']==str(phase) and r['label']==label and r['operation']=='StopCapture'),None)
        active=calls[first:last+1] if first is not None and last is not None else []
        expected_counts=dict(SetReceiving=1,StartCapture=1,WaitFrame=1,IsFinishedReceiving=1,CloseDMAEvent=1,
                             ClearVsyncEvent=4,WaitVsync=4,GetLatestVsyncTiming=12,HGPhaseDelay=4,StopCapture=1)
        for op,count in expected_counts.items():
            rs=[r for r in active if r['operation']==op]
            if len(rs)!=count or any(r['sensor']!='1' or r['result']!='00000000' for r in rs):bad.append('bad_call:'+op)
        if len(chosen('SetReceiving'))!=1 or len(chosen('StartCapture'))!=1:bad.append('extra_receive_or_start')
        allowed=set(expected_counts)|{'ReadRegisterI2cExclusive','ReadMcuVariableI2cExclusive','WriteRegisterI2c'}
        if any(r['operation'] not in allowed for r in active):bad.append('unregistered_call_in_window')
        if any(r['result']!='00000000' for r in active):bad.append('failed_call_in_window')
        writes=[r for r in active if r['operation']=='WriteRegisterI2c']
        if len(writes)!=16 or any(r['label']!='hg_trial_rewrite' or r['addr']!=IFP[i%4] or r['value']!=str(rule['timing']['rewrite_targets'][i//4] if ccm else 128) for i,r in enumerate(writes)):bad.append('unexpected_window_writes')
        order=[r['operation'] for r in active if r['operation'] in ('WaitFrame','IsFinishedReceiving','CloseDMAEvent','WriteRegisterI2c')]
        if order!=['WaitFrame','IsFinishedReceiving','CloseDMAEvent']+['WriteRegisterI2c']*16:bad.append('DMA_not_closed_before_rewrite')
        wait=[r for r in active if r['operation']=='WaitFrame'];closed=[r for r in active if r['operation']=='CloseDMAEvent']
        if len(wait)!=1 or len(closed)!=1 or wait[0]['value']!=closed[0]['value']:bad.append('wrong_DMA_handle')
        if any(r['value']!='1' for r in active if r['operation']=='IsFinishedReceiving'):bad.append('pending_DMA')
        acquired=chosen('GetVsyncInterruptEvent');released=chosen('CloseVsyncEvent')
        if len(acquired)!=1 or len(released)!=1 or acquired[0]['value']=='0' or acquired[0]['value']!=released[0]['value'] or any(r['result']!='00000000' for r in acquired+released):bad.append('VSYNC_handle')
        elif any(r['value']!=acquired[0]['value'] for r in active if r['operation'] in ('ClearVsyncEvent','WaitVsync')):bad.append('wrong_VSYNC_handle')
        if [int(r['value']) for r in active if r['operation']=='HGPhaseDelay']!=list(HG_DELAYS):bad.append('phase_delay_plan')
        if [r['operation'] for r in active if r['operation'] in ('GetLatestVsyncTiming','ClearVsyncEvent','WaitVsync','HGPhaseDelay')]!=['GetLatestVsyncTiming','ClearVsyncEvent','WaitVsync','GetLatestVsyncTiming','HGPhaseDelay','GetLatestVsyncTiming']*4:bad.append('VSYNC_operation_order')
        if any(r['value']!='1' for r in active if r['operation']=='GetLatestVsyncTiming'):bad.append('VSYNC_timing_count')
        trial_results=[];previous=0
        for trial,delay in enumerate(HG_DELAYS):
            target=rule['timing']['rewrite_targets'][trial] if ccm else 128
            if ccm:
                targets=[t for t in manifest_records(p,'CCM_TRIAL_TARGET') if t.get('phase')==str(phase) and t.get('trial')==str(trial)]
                if targets!=[dict(phase=str(phase),trial=str(trial),target=str(target))]:bad.append('CCM_trial_target_missing')
            vs=[r for r in manifest_records(p,'HG_VSYNC') if r.get('phase')==str(phase) and r.get('trial')==str(trial)]
            end=[r for r in manifest_records(p,'HG_TRIAL') if r.get('phase')==str(phase) and r.get('trial')==str(trial)]
            poll=[r for r in manifest_records(p,'HG_POLL') if r.get('phase')==str(phase) and r.get('trial')==str(trial)]
            red=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['frame']==str(trial) and r['name']=='hg_red']
            rb=[r for r in reads if r['phase']==str(phase) and r['label']==label and r['frame']==str(trial) and r['name']=='hg_rewrite']
            valid=len(vs)==len(end)==1 and 1<=len(poll)<=64 and len(red)==len(poll)
            if valid:
                v=vs[0];e=end[0];baseline=int(v['baseline']);latest=int(v['latest']);ret=int(v['returned_ms'])
                wb=int(e['write_begin_ms']);we=int(e['write_end_ms'])
                valid=v['verified']=='1' and e['complete']=='1' and 0<baseline<latest and latest>previous and int(e['after_raw'])>=latest
                valid=valid and e['delay_ms']==str(delay) and int(e['polls'])==len(poll) and ret+delay<=wb<=we
                for j,(q,r) in enumerate(zip(poll,red)):
                    b=int(q['begin_ms']);t=int(q['end_ms']);value=int(q['value'])
                    valid=valid and q['index']==str(j) and q['verified']=='1' and we<=b<=int(r['time_ms'])<=t and r['value']==q['value'] and r['result']=='00000000' and r['addr']=='32d4' and r['sensor']=='1' and r['space']=='reg' and r['when']=='poll' and 1<=value<=512
                    if j and b<int(poll[j-1]['end_ms']):valid=False
                    if j<len(poll)-1 and value!=target:valid=False
                valid=valid and e['last']==poll[-1]['value'] and (e['last']!=str(target) or len(poll)==64)
                previous=latest
            if not valid:bad.append('trial_proof:'+str(trial))
            if len(rb)!=4 or [r['addr'] for r in rb]!=IFP or any(r['space']!='reg' or r['sensor']!='1' or r['result']!='00000000' or r['when']!='rewrite_readback' or not 1<=int(r['value'])<=512 for r in rb):bad.append('rewrite_readback:'+str(trial))
            observed=[int(r['value']) for r in red]
            first_changed=next((j for j,x in enumerate(observed) if x!=target),None)
            bracket=None
            if first_changed is not None and len(poll)==len(red):
                lo=int(poll[first_changed-1]['begin_ms']) if first_changed else None
                bracket={('last_target_read_begin_ms' if ccm else 'last_unity_read_begin_ms'):lo,'first_changed_read_end_ms':int(poll[first_changed]['end_ms']),
                             'since_write_end_upper_ms':int(poll[first_changed]['end_ms'])-int(end[0]['write_end_ms']) if len(end)==1 else None}
            trial_results.append(dict(trial=trial,delay_ms=delay,write_target=target,rewrite_readback=[int(r['value']) for r in rb],poll_values=observed,
                                      overwritten=first_changed is not None,transition_bracket=bracket,vsync=vs,summary=end))
        captures=[r for r in frames if r['phase']==str(phase)]
        if len(captures)!=1 or any(r['label']!=label or r['frame']!='0' or r['measured']!='0' or r['ok']!='1' or r['received']!='1' or r['result']!='00000000' for r in captures):bad.append('initial_DMA_frame')
        if dict(phase=str(phase),complete='1',trials='4',raw_provenance='first_DMA_only') not in manifest_records(p,'HG_WINDOW'):bad.append('window_incomplete')
        raw=p/f's1-p{phase:03d}-{label}.yuyv'
        record=[r for r in manifest_records(p,'SWEEP_RAW') if r.get('phase')==str(phase)]
        if raw.name not in names or not raw.is_file() or raw.stat().st_size!=24576 or len(record)!=1 or record[0].get('crc32')!=f'{zlib.crc32(raw.read_bytes()):08x}':bad.append('ROI_missing_or_changed')
        changed={a:[snapshots['before_stream'].get(a),v] for a,v in snapshots['after_stop'].items() if snapshots['before_stream'].get(a)!=v}
        arms.append(dict(index=index,enabled=enabled,target=target,groups=groups,trials=trial_results,snapshots=snapshots,snapshot_changes=changed,errors=bad))
        errors.extend(f'arm{index}:{e}' for e in bad)
    if len(manifest_records(p,'CCM_TRIAL_TARGET'))!=(12 if ccm else 0):errors.append('unexpected_CCM_trial_targets')
    if len(manifest_records(p,'CCM_MODE'))!=(3 if ccm else 0):errors.append('unexpected_CCM_mode_records')
    # Global whitelists and exact Refresh count prevent local bypasses.
    for r in calls:
        if r['operation']=='WriteMcuVariableI2c' and r['addr'] not in ('a102','a103','a11f','a355'):errors.append('unregistered_MCU_write')
        if r['operation']=='WriteRegisterI2c' and r['addr'] not in IFP+['3012','3056','3058','305a','305c']:errors.append('unregistered_physical_write')
    for address,count in (('a102',10),('a11f',7),('a355',5 if ccm else 2),('a103',4)):
        chosen=[r for r in calls if r['operation']=='WriteMcuVariableI2c' and r['addr']==address]
        if len(chosen)!=count or any(r['result']!='00000000' for r in chosen):errors.append('MCU_write_budget_or_failure:'+address)
    refreshed=manifest_records(p,'SEQ_REFRESH')
    if len(refreshed)!=4 or [r.get('restoring') for r in refreshed]!=['0']*3+['1'] or any(r.get('verified')!='1' or r.get('state')!='3' or not 2<=int(r.get('polls','0'))<=100 for r in refreshed):errors.append('Refresh_completion')
    for label in ('PREVIEW_AWB_RESTORE','SEQ_RESTORE','AWB_RESTORE'):
        rs=manifest_records(p,label);mask=96 if label=='AWB_RESTORE' else 65535
        if len(rs)!=1 or rs[0].get('verified')!='1' or (int(rs[0].get('saved','-1'))&mask)!=(int(rs[0].get('actual','-2'))&mask):errors.append('restore:'+label)
    original={}
    for label in ('sweep_original','sweep_restore_controls'):
        rs=[r for r in reads if r['label']==label and r['name']=='calibration_saved']
        if len(rs)!=5 or any(r['result']!='00000000' for r in rs):errors.append('physical_restore_missing')
        values={r['addr']:r['value'] for r in rs}
        if label=='sweep_original':original=values
        elif values!=original:errors.append('physical_restore_mismatch')
    # The stored reference ISP is the original state to which cleanup returns.
    restore=[r for r in reads if r['label']=='sweep_restore_isp' and r['when']=='verified']
    pinned=[r for r in reads if r['label']=='sweep_ref_00' and r['when']=='isp_pin']
    if len(restore)!=4 or len(pinned)!=4 or {r['addr'] for r in restore}!=set(IFP) or any(r['result']!='00000000' for r in restore+pinned):errors.append('IFP_restore_missing')
    elif {r['addr']:r['value'] for r in restore}!={r['addr']:r['value'] for r in pinned}:errors.append('IFP_restore_mismatch')
    end=manifest_records(p,'END')
    if len(end)!=1 or any(end[0].get(k)!=v for k,v in dict(complete='1',aborted='0',io_error='0').items()):errors.append('run_incomplete')
    if manifest_records(p,'HG_END')!=[dict(complete='1',arms='3',restored='1')]:errors.append('experiment_incomplete')
    conclusion='inconclusive'
    patterns=[[t['overwritten'] for t in a['trials']] for a in arms]
    if not errors:
        conclusion='HG_gate_dependency_supported' if patterns==[[True]*4,[False]*4,[True]*4] else 'HG_gate_not_sufficient_to_stop_update' if all(all(x) for x in patterns) else 'no_overwrite_observed' if not any(any(x) for x in patterns) else 'mixed_requires_followup'
    if ccm and not errors:
        endpoints=[[[g['values'].get(a) for a in IFP] for g in arm['groups'] if g['boundary']=='after_poll'] for arm in arms]
        normalized=endpoints[0]==endpoints[2]==[[133]*4]*4
        if normalized and endpoints[1]==[[128]*4]*4:conclusion='CCM_bit6_changes_reload_value_to_unity'
        elif normalized and endpoints[1]==[[v]*4 for v in rule['timing']['rewrite_targets']]:conclusion='CCM_bit6_suppresses_observed_update'
        elif endpoints==[[[133]*4]*4]*3:conclusion='CCM_bit6_not_sufficient_to_stop_update'
        elif not any(any(x) for x in patterns):conclusion='no_overwrite_observed'
        else:conclusion='mixed_requires_followup'
    return dict(schema='camera-ccm-normalization-report/v3' if boot else 'camera-ccm-normalization-report/v2' if byte_words else 'camera-ccm-normalization-report/v1' if ccm else 'camera-hg-isolation-report/v1',source=source,passed=not errors,calibration_validated=False,
                files_verified=len(receipts),errors=errors,arms=arms,conclusion=conclusion,writer_identified=False,
                timing_cause_identified=False,atomic_snapshot=False,raw_provenance='initial_DMA_only',startup_journal=startup)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);parser.add_argument('--out',required=True,type=Path)
    args=parser.parse_args();r=analyse(args.directory)
    with args.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps(dict(passed=r['passed'],conclusion=r['conclusion'],errors=r['errors'],files=r['files_verified'])))
    raise SystemExit(0 if r['passed'] else 2)
