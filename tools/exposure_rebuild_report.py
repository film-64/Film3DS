#!/usr/bin/env python3
"""Verify target-specific exposure reconstruction evidence; never certify ISO."""
import argparse
from collections import defaultdict,Counter
import hashlib,json,math,re,statistics,zlib
from pathlib import Path
from decode_exposure_state import read_csv
from exposure_session import manifest_records
from exposure_sweep_contract import CONTRACT_MATRIX,EXPOSURE_REBUILD_SOURCE,REBUILD_COARSE,REBUILD_CORES,REBUILD_ISP,REBUILD_COLORS,BOOT_STAGES,EXPOSURE_DENSE_SOURCE,DENSE_COARSE
RULE=CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE]
IFP=['32d4','32d6','32d8','32da'];CORE=['3012','3056','3058','305a','305c']

def analyse(directory,expected_source=EXPOSURE_REBUILD_SOURCE):
    if expected_source not in (EXPOSURE_REBUILD_SOURCE,EXPOSURE_DENSE_SOURCE):raise ValueError('unregistered report input source')
    dense=expected_source==EXPOSURE_DENSE_SOURCE;RULE=CONTRACT_MATRIX[expected_source];total_DMA=444 if dense else 748;first_block_points=39 if dense else 38;first_block_DMA=234 if dense else 380
    p=Path(directory);receipts=json.loads((p/'received-ram-sha256.json').read_text());names=[e['name'] for e in receipts]
    required={'manifest.txt','reads.csv','calls.csv','frames.csv','rebuild_points.csv','rebuild_frames.csv','rebuild_bands.csv','startup_journal.txt'}
    if len(names)!=len(set(names)) or not required.issubset(names):raise ValueError('missing or duplicate original receipt')
    for e in receipts:
        if Path(e['name']).name!=e['name']:raise ValueError('invalid receipt path')
        b=(p/e['name']).read_bytes()
        if len(b)!=e['bytes'] or hashlib.sha256(b).hexdigest()!=e['sha256'] or f'{zlib.crc32(b):08x}'!=e['crc32']:raise ValueError('original integrity failure:'+e['name'])
    tags=['REBUILD_REASSERT','REBUILD_COLOR_MATRIX','REBUILD_CONTRACT','REBUILD_BUFFER','REBUILD_BLOCK','REBUILD_BLOCK_END','REBUILD_POINT','REBUILD_POINT_END','REBUILD_ANCHOR','REBUILD_END','REBUILD_TIMING','MATRIX_SNAPSHOT','CCM_WORD','CCM_ALIAS_WINDOW','MATRIX_ALIAS_TRANSPORT','SEQ_REFRESH','PREVIEW_AWB_RESTORE','AWB_RESTORE','SEQ_RESTORE','SWEEP_RAW','END']
    rec={t:manifest_records(p,t) for t in tags}
    if rec['REBUILD_CONTRACT']!=[dict(source=expected_source,blocks='2',points='74',DMA=str(total_DMA),images='296',calibration='0',optical_time='0')]:raise ValueError('unregistered rebuild source')
    reads=read_csv(p/'reads.csv');calls=read_csv(p/'calls.csv');points=read_csv(p/'rebuild_points.csv');frames=read_csv(p/'rebuild_frames.csv');bands=read_csv(p/'rebuild_bands.csv');legacy=read_csv(p/'frames.csv')
    ri=defaultdict(list);ci=defaultdict(list);fi=defaultdict(list);bi=defaultdict(list);li=defaultdict(list)
    for r in reads:ri[r['label'],r['frame'],r['when'],r['name']].append(r)
    for i,r in enumerate(calls):ci[r['label']].append((i,r))
    for r in frames:fi[r['point']].append(r)
    for r in bands:bi[r['point'],r['frame']].append(r)
    for r in legacy:li[r['label'],r['frame']].append(r)
    errors=[];results=[];blocks=[]
    for tag,n in [('REBUILD_BLOCK',2),('REBUILD_BLOCK_END',2),('REBUILD_POINT',74),('REBUILD_POINT_END',74),('REBUILD_ANCHOR',2),('REBUILD_REASSERT',0 if dense else 2),('MATRIX_SNAPSHOT',4),('REBUILD_TIMING',4),('CCM_WORD',44),('CCM_ALIAS_WINDOW',4),('MATRIX_ALIAS_TRANSPORT',4)]:
        if len(rec[tag])!=n:errors.append('record_count:'+tag)
    if len(points)!=74 or [r['point'] for r in points]!=list(map(str,range(74))):errors.append('point_count_or_order')
    if len(frames)!=total_DMA or [r['ordinal'] for r in frames]!=list(map(str,range(total_DMA))):errors.append('DMA_count_or_order')
    if len(bands)!=total_DMA or [(r['point'],r['frame']) for r in bands]!=[(r['point'],r['frame']) for r in frames]:errors.append('band_count_or_order')
    if rec['REBUILD_BUFFER']!=[dict(extra_bytes='614400',slots='2',max_pending='1',allocated='1',crc_scratch='24576')]:errors.append('buffer_registration')
    saved_rows=ri['isp_update_save','-1','saved','seq_mode']+ri['isp_update_save','-1','saved','awb_mode']+ri['isp_update_save','-1','saved','preview_awb']
    saved={r['addr']:int(r['value']) for r in saved_rows}
    if len(saved_rows)!=3 or set(saved)!={'a102','a355','a11f'} or any(r['result']!='00000000' or r['sensor']!='1' for r in saved_rows):errors.append('saved_MCU')
    matrix_records=rec['REBUILD_COLOR_MATRIX'];matrix_calls=[r for _,r in ci['rb_color_matrix'] if r['operation']=='GetSuitableY2rStandardCoefficient'];matrix_value=int(matrix_records[0].get('value','-1')) if len(matrix_records)==1 else -1
    if matrix_value not in RULE['color_conversion']['allowed'] or matrix_records!=[dict(value=str(matrix_value),verified='1')] or len(matrix_calls)!=1 or matrix_calls[0]['value']!=str(matrix_value) or matrix_calls[0]['result']!='00000000':errors.append('color_conversion_proof')
    point_by_index={int(r['point']):r for r in points};global_ordinal=0
    def snapshot(block,phase,when,bad):
        label=f'rb_block{block}';rs=ri[label,'-1',when,'hg_snapshot'];expected={('mcu',a) for a in RULE['snapshot']['mcu']}|{('reg',a) for a in RULE['snapshot']['physical']}
        if len(rs)!=len(expected) or {(r['space'],r['addr']) for r in rs}!=expected or any(r['phase']!=str(phase) or r['sensor']!='1' or r['result']!='00000000' or (r['space']=='mcu' and int(r['addr'],16)&0x8000 and int(r['value'])>255) for r in rs):bad.append('snapshot:'+when)
        values={r['addr']:int(r['value']) for r in rs};br=ri[label,'-1',when,'ccm_byte'];raw={r['addr']:int(r['value']) for r in br};aliases={f'{(int(a,16)|0x8000)+o:04x}' for a in RULE['snapshot']['derived_words'] for o in (0,1)}
        valid=len(br)==22 and set(raw)==aliases and all(r['result']=='00000000' and r['space']=='mcu' and r['sensor']=='1' and r['phase']==str(phase) and 0<=int(r['value'])<=255 for r in br)
        words=[r for r in rec['CCM_WORD'] if r.get('phase')==str(phase) and r.get('when')==when]
        if len(words)!=11 or {r.get('addr') for r in words}!=set(RULE['snapshot']['derived_words']):valid=False
        if valid:
            for w in words:
                a=w['addr'];hi=raw[f'{int(a,16)|0x8000:04x}'];lo=raw[f'{(int(a,16)|0x8000)+1:04x}'];v=(hi<<8)|lo;values[a]=v
                if w.get('msb')!=str(hi) or w.get('lsb')!=str(lo) or w.get('value')!=str(v):valid=False
        if not valid:bad.append('byte_transport:'+when)
        ar=[]
        for name in ('matrix_alias_native_before','matrix_alias_hi','matrix_alias_lo','matrix_alias_native_after'):ar+=ri[label,'-1',when,name]
        if len(ar)!=4 or [r['addr'] for r in ar]!=['2703','a703','a704','2703'] or [r['value'] for r in ar]!=['640','2','128','640'] or any(r['result']!='00000000' or r['sensor']!='1' or r['space']!='mcu' or r['phase']!=str(phase) for r in ar):bad.append('static_alias_proof:'+when)
        if [r for r in rec['MATRIX_ALIAS_TRANSPORT'] if r.get('phase')==str(phase) and r.get('when')==when]!=[dict(phase=str(phase),when=when,before='640',combined='640',after='640',verified='1')]:bad.append('static_alias_marker:'+when)
        before=ri[label,'-1',when,'ccm_native_reference'];after=ri[label,'-1',when,'ccm_native_after'];window=[r for r in rec['CCM_ALIAS_WINDOW'] if r.get('phase')==str(phase) and r.get('when')==when]
        if len(before)!=1 or len(after)!=1 or any(r['addr']!='2332' or r['result']!='00000000' or r['sensor']!='1' or r['space']!='mcu' for r in before+after) or not valid:bad.append('CCM_reference:'+when)
        else:
            v=values['2332'];a=int(before[0]['value']);z=int(after[0]['value']);expected_window=dict(phase=str(phase),when=when,addr='2332',before=str(a),combined=str(v),after=str(z),match_before=str(int(a==v)),match_after=str(int(z==v)))
            if window!=[expected_window]:bad.append('CCM_coherence:'+when)
            values['ccm_snapshot_coherence']=dict(**expected_window,atomic=False)
        for name,addresses,space in [('matrix_mode',RULE['snapshot']['Mode_readonly'],'mcu'),('rebuild_timing',RULE['timing_snapshot_registers'],'reg')]:
            group=ri[label,'-1',when,name]
            if len(group)!=len(addresses) or {r['addr'] for r in group}!=set(addresses) or any(r['phase']!=str(phase) or r['sensor']!='1' or r['space']!=space or r['result']!='00000000' for r in group):bad.append(name+':'+when)
            values[name]={r['addr']:int(r['value']) for r in group}
        if dict(node=str(block),when=when,complete='1') not in rec['MATRIX_SNAPSHOT'] or dict(block=str(block),when=when,complete='1') not in rec['REBUILD_TIMING']:bad.append('snapshot_completion:'+when)
        return values
    for block,fps in enumerate((5,15)):
        start=0 if block==0 else first_block_points;end=first_block_points if block==0 else 74;block_bad=[];base={};coarse=(DENSE_COARSE if dense else REBUILD_COARSE)[fps]
        if dict(index=str(block),fps=str(fps),first_point=str(start)) not in rec['REBUILD_BLOCK'] or dict(index=str(block),complete='1') not in rec['REBUILD_BLOCK_END']:block_bad.append('block_identity')
        before_snap=snapshot(block,start+4,'before_stream',block_bad)
        coarse_rows=[point_by_index.get(start+1+i,{}) for i in range(len(coarse))];scores=[]
        for row in coarse_rows:
            scores.append(float(row.get('score_y','nan')))
        eligible=[i for i,x in enumerate(scores) if math.isfinite(x) and 16<=x<=224]
        primary=min(eligible,key=lambda i:abs(scores[i]-(80 if dense else 128))) if eligible else 5;secondary=max(0,primary-2);coverage=bool(eligible)
        if dense:
            ts=before_snap.get('rebuild_timing',{});fine=ts.get('3014',0)/max(1,ts.get('300c',0));second=[i for i in eligible if (coarse[i]+fine)**2>=2*(coarse[primary]+fine)**2]
            coverage=coverage and bool(second)
            if coverage:secondary=min(second,key=lambda i:abs(scores[i]-120))
            else:primary,secondary=5,9
        if dict(block=str(block),primary=str(primary),secondary=str(secondary),coverage=str(int(coverage))) not in rec['REBUILD_ANCHOR']:block_bad.append('adaptive_anchor')
        plan=[('reference_start',128,528,[128]*4,6,0,-1,True)]
        plan += [('coarse',v,528,[128]*4,6,0,i,True) for i,v in enumerate(coarse)]
        plan += [('gain',coarse[a],c,[d]*4,6,0,a,coverage) for a in (primary,secondary) for c in REBUILD_CORES for d in REBUILD_ISP]
        if not dense:
            plan += [('color',coarse[primary],528,list(v),6,0,primary,coverage) for v in REBUILD_COLORS]
            plan += [('hold',coarse[primary],528,[128]*4,128,0,primary,coverage)]
        plan += [('reference_end',128,528,[128]*4,6,0,-1,True)]
        if not dense:plan += [(kind,4,528,[128]*4,16,gate,-1,True) for kind,gate in [('FD_off_before',0),('FD_on',2),('FD_off_after',0)]]
        block_frames=[];point_results=[]
        for index,target in enumerate(plan,start):
            kind,ct,core,isp,n,gate,anchor,cov=target;phase=index+5;label=f'rb_p{index:03d}';bad=[];row=point_by_index.get(index,{})
            expected=dict(point=str(index),phase=str(phase),block=str(block),kind=kind,fps=str(fps),gate=str(gate),coarse=str(ct),core=str(core),isp_r=str(isp[0]),isp_gr=str(isp[1]),isp_gb=str(isp[2]),isp_b=str(isp[3]),frames=str(n),anchor=str(anchor),coverage=str(int(cov)),complete='1')
            if any(row.get(k)!=v for k,v in expected.items()):bad.append('registered_point_plan')
            header=dict(index=str(index),**{k:v for k,v in expected.items() if k not in ('point','complete')});header['isp']=','.join(map(str,isp))
            for k in ('isp_r','isp_gr','isp_gb','isp_b'):header.pop(k,None)
            if header not in rec['REBUILD_POINT'] or dict(index=str(index),complete='1') not in rec['REBUILD_POINT_END']:bad.append('point_manifest')
            lo=int(row.get('write_begin_ms','0'));hi=int(row.get('write_end_ms','0'))
            wr=[r for _,r in ci[label] if r['operation']=='WriteRegisterI2c']
            if [r['addr'] for r in wr]!=CORE+IFP or [r['value'] for r in wr]!=list(map(str,[ct]+[core]*4+isp)) or any(r['sensor']!='1' or r['phase']!=str(phase) or r['result']!='00000000' or not lo<=int(r['time_ms'])<=hi for r in wr):bad.append('point_writes')
            def state(frame,when):
                rs=ri[label,str(frame),when,'rebuild_state'];vs={r['addr']:int(r['value']) for r in rs}
                fixed={'a102':(saved.get('a102',0)&~15)|gate,'a103':0,'a104':3,'a34e':128,'a34f':128,'a350':128,'3012':ct,'300a':2114 if fps==5 else 704,**{a:core for a in CORE[1:]}}
                valid=len(rs)==len(RULE['state_registers']) and set(vs)==set(RULE['state_registers']) and all(r['sensor']=='1' and r['phase']==str(phase) and r['result']=='00000000' and r['space']==('mcu' if r['addr'].startswith('a') or r['addr']=='221f' else 'reg') for r in rs)
                if gate:fixed.pop('3012')
                valid=valid and (not gate or 1<=vs.get('3012',0)<=(1585 if fps==5 else 528)) and all(vs.get(a)==v for a,v in fixed.items()) and vs.get('a355',0)&96==32 and 1<=vs.get('221f',0)<=512 and vs.get('300c',0)>0
                if not base and valid:base.update({a:vs[a] for a in ('221f','300c','3014')})
                valid=valid and all(vs.get(a)==v for a,v in base.items()) and all(1<=vs.get(a,0)<=512 and (kind.startswith('FD_') or vs.get(a)==v) for a,v in zip(IFP,isp))
                times=[int(r['time_ms']) for r in rs]
                if not valid or times!=sorted(times):bad.append(f'state:{frame}:{when}')
                return vs,times
            state(-1,'configured');fs=fi[str(index)];raw_records=[r for r in rec['SWEEP_RAW'] if r.get('phase')==str(phase)]
            if len(fs)!=n or [r['frame'] for r in fs]!=list(map(str,range(n))) or len(raw_records)!=4:bad.append('point_frame_count')
            samples=[];observed=defaultdict(set)
            for j,f in enumerate(fs):
                ordinal=int(f['ordinal']);event=f['event'];slot=f['slot'];block_frames.append(f)
                save=j in (0,1,n-2,n-1)
                if ordinal!=global_ordinal or slot!=str(ordinal%2) or f['point']!=str(index) or f['phase']!=str(phase) or f['saved']!=str(int(save)) or f['write_begin_ms']!=str(lo) or f['write_end_ms']!=str(hi) or int(f['vsync_raw'])<=0:bad.append('frame_identity')
                global_ordinal+=1
                bv,bt=state(j,'before');av,at=state(j,'after')
                for a in IFP+['3012']:observed[a].add(av.get(a))
                bms,wms,ams=map(int,(f['before_ms'],f['wait_ms'],f['after_ms']))
                if not lo<=hi<=bms<=wms<=ams or not bt or not at or min(bt)<bms or max(bt)>wms or min(at)<wms or max(at)>ams:bad.append('frame_state_time')
                dma=ci[f'rb_dma_{ordinal:04d}'];ops=[r['operation'] for _,r in dma];expected_ops=['FlushDataCache','SetReceiving','WaitFrame','IsFinishedReceiving','CloseDMAEvent','InvalidateDataCache','GetLatestVsyncTiming']
                if ops!=expected_ops or any(r['sensor']!='1' or r['result']!='00000000' for _,r in dma):bad.append('DMA_operations')
                else:
                    dm={r['operation']:r for _,r in dma}
                    if dm['FlushDataCache']['value']!=slot or dm['SetReceiving']['value']!=slot or dm['InvalidateDataCache']['value']!=slot or dm['WaitFrame']['value']!=event or dm['CloseDMAEvent']['value']!=event or event=='0' or dm['IsFinishedReceiving']['value']!='1' or int(dm['WaitFrame']['time_ms'])<wms or int(dm['CloseDMAEvent']['time_ms'])>ams:bad.append('DMA_ownership_or_completion')
                band=bi[str(index),str(j)]
                if len(band)!=1 or band[0]['phase']!=str(phase) or band[0]['ordinal']!=str(ordinal):bad.append('band_identity')
                else:
                    ys=[float(band[0][f'b{k:02d}']) for k in range(16)]
                    if any(not math.isfinite(v) or not 0<=v<=255 for v in ys) or abs(statistics.mean(ys)-float(f['y_mean']))>1e-6:bad.append('band_mean')
                if save:
                    name=f's1-p{phase:03d}-{label}_f{j:03d}.yuyv';raw=p/name
                    if name not in names or not raw.is_file():bad.append('missing_target_image')
                    else:
                        data=raw.read_bytes();y=list(data[::2]);crc=f'{zlib.crc32(data):08x}'
                        if len(data)!=24576 or crc!=f['crc32']:bad.append('image_crc_or_size')
                        else:
                            mean=sum(y)/len(y);variance=sum(v*v for v in y)/len(y)-mean*mean
                            if abs(mean-float(f['y_mean']))>1e-6 or abs(variance-float(f['y_variance']))>1e-5 or sum(v<=4 for v in y)!=int(f['low_count']) or sum(v>=250 for v in y)!=int(f['high_count']):bad.append('image_statistics')
                            if len(band)==1 and any(abs(sum(y[k*768:(k+1)*768])/768-float(band[0][f'b{k:02d}']))>1e-6 for k in range(16)):bad.append('image_bands')
                            pos=(0,1,n-2,n-1).index(j)
                            if len(raw_records)==4 and raw_records[pos].get('crc32')!=crc:bad.append('raw_manifest')
                    samples.append(name)
                lr=li[label,str(j)]
                if len(lr)!=1 or any(lr[0].get(k)!=v for k,v in dict(sensor='1',phase=str(phase),ok='1',received='1',result='00000000',crc32=f['crc32']).items()):bad.append('legacy_frame_correspondence')
            if len(fs)>=2 and abs((float(fs[-1]['y_mean'])+float(fs[-2]['y_mean']))/2-float(row.get('score_y','nan')))>1e-6:bad.append('adaptive_score_not_actual_images')
            reassert_hold=None
            if kind=='FD_off_after':
                markers=[x for x in rec['REBUILD_REASSERT'] if x.get('point')==str(index)]
                iw=[x for _,x in ci[f'rb_reassert_p{index:03d}'] if x['operation']=='WriteRegisterI2c']
                valid=len(markers)==1 and markers[0].get('phase')==str(phase) and markers[0].get('frame')=='8' and markers[0].get('complete')=='1' and len(fs)==16 and [x['addr'] for x in iw]==IFP and all(x['value']=='128' and x['result']=='00000000' and x['sensor']=='1' and x['phase']==str(phase) for x in iw)
                if valid:
                    a,z=int(markers[0]['begin_ms']),int(markers[0]['end_ms']);valid=int(fs[7]['after_ms'])<=a<=z<=int(fs[8]['before_ms']) and all(a<=int(x['time_ms'])<=z for x in iw)
                    reassert_hold=all(len([v for v in ri[label,str(j),'after','rebuild_state'] if v['addr'] in IFP])==4 and all(v['value']=='128' for v in ri[label,str(j),'after','rebuild_state'] if v['addr'] in IFP) for j in range(8,16))
                if not valid:bad.append('planned_reassert_proof')
            point_results.append(dict(FD_after_reassert_held=reassert_hold,index=index,phase=phase,kind=kind,fps=fps,gate=gate,coarse=ct,core=core,ISP_target=isp,frames=n,score_y=float(row.get('score_y','nan')),anchor=anchor,coverage=cov,observed_IFP={a:sorted(v for v in observed[a] if v is not None) for a in IFP},observed_coarse=sorted(v for v in observed['3012'] if v is not None),first_two_means=[float(x['y_mean']) for x in fs[:2]],last_two_means=[float(x['y_mean']) for x in fs[-2:]],images=samples,errors=bad))
            errors.extend(f'point{index}:{e}' for e in bad)
        after_snap=snapshot(block,end+4,'after_stop',block_bad)
        first_ordinal=0 if block==0 else first_block_DMA;last_ordinal=first_block_DMA-1 if block==0 else total_DMA-1
        for ordinal in range(first_ordinal,last_ordinal):
            current=ci[f'rb_dma_{ordinal:04d}'];next_=ci[f'rb_dma_{ordinal+1:04d}']
            def op_index(group,op):return next((i for i,r in group if r['operation']==op),-1)
            if not 0<=op_index(current,'CloseDMAEvent')<op_index(next_,'SetReceiving')<op_index(current,'GetLatestVsyncTiming')<op_index(next_,'WaitFrame'):block_bad.append('pending_receive_order')
        starts=ci[f'rb_start_b{block}'];stops=ci[f'rb_stop_b{block}'];start_calls=[(i,r) for i,r in starts if r['operation']=='StartCapture'];stop_calls=[(i,r) for i,r in stops if r['operation']=='StopCapture']
        if len(start_calls)!=1 or len(stop_calls)!=1 or start_calls[0][1]['result']!='00000000' or stop_calls[0][1]['result']!='00000000':block_bad.append('stream_start_stop')
        else:
            a=next((i for i,r in ci[f'rb_dma_{first_ordinal:04d}'] if r['operation']=='SetReceiving'),-1);z=next((i for i,r in ci[f'rb_dma_{last_ordinal:04d}'] if r['operation']=='GetLatestVsyncTiming'),-1)
            if not a<start_calls[0][0]<next((i for i,r in ci[f'rb_dma_{first_ordinal:04d}'] if r['operation']=='WaitFrame'),-1) or stop_calls[0][0]<=z:block_bad.append('stream_order')
            active=calls[start_calls[0][0]+1:stop_calls[0][0]];point_labels={f'rb_p{i:03d}' for i in range(start,end)}|{f'rb_reassert_p{end-1:03d}'}
            for call in active:
                op=call['operation']
                if op not in RULE['stream_allowed_operations'] or call['result']!='00000000':block_bad.append('unregistered_or_failed_stream_operation')
                if op=='WriteMcuVariableI2c' and (call['label']!='rb_gate' or call['addr']!='a102'):block_bad.append('unregistered_stream_MCU_write')
                if op=='WriteRegisterI2c' and call['label'] not in point_labels:block_bad.append('unregistered_stream_physical_write')

        host_steps=[int(b['wait_ms'])-int(a['wait_ms']) for a,b in zip(block_frames[2:],block_frames[3:])];raw_steps=[int(b['vsync_raw'])-int(a['vsync_raw']) for a,b in zip(block_frames,block_frames[1:])]
        median_ms=statistics.median(host_steps) if host_steps else 0;timing_usable=bool(host_steps and raw_steps and min(raw_steps)>0 and abs(median_ms-1000/fps)<=200/fps)
        for x in point_results:x['estimated_seconds']=(x['coarse']+base.get('3014',0)/max(1,base.get('300c',0)))*median_ms/1000/(2114 if fps==5 else 704) if timing_usable and x['gate']==0 else None
        changed={a:[before_snap.get(a),after_snap.get(a)] for a in RULE['snapshot']['physical'] if before_snap.get(a)!=after_snap.get(a)}
        blocks.append(dict(index=block,fps=fps,primary_anchor=primary,secondary_anchor=secondary,anchor_coverage=coverage,baseline=base,median_DMA_interval_ms=median_ms,raw_VSYNC_steps_positive=bool(raw_steps and min(raw_steps)>0),timing_estimate_usable=timing_usable,pipeline_endpoint_changes=changed,snapshots=dict(before_stream=before_snap,after_stop=after_snap),points=point_results,errors=block_bad));results+=point_results;errors.extend(f'block{block}:{e}' for e in block_bad)
    if global_ordinal!=total_DMA:errors.append('global_DMA_count')
    raw_names={name for x in results for name in x['images']}
    if len(raw_names)!=296 or {n for n in names if '-rb_p' in n and n.endswith('.yuyv')}!=raw_names:errors.append('extra_or_missing_image')
    if any(int(r['phase'])>78 for r in reads+calls+legacy):errors.append('unknown_phase')
    for r in calls:
        if int(r['phase'])>=5 and (r['operation'] not in RULE['post_seed_allowed_operations'] or r['result']!='00000000'):errors.append('unregistered_or_failed_post_seed_operation')
        if r['operation']=='SetFrameRate' and int(r['phase'])>=5 and r['label'] not in ('rb_profile15','rb_profile_restore5'):errors.append('unregistered_profile_call')
        if r['operation']=='WriteMcuVariableI2c' and r['addr'] not in ('a102','a103','a11f','a355'):errors.append('unknown_MCU_write')
        if r['operation']=='WriteRegisterI2c' and r['addr'] not in CORE+IFP:errors.append('unknown_register_write')
        if r['operation'] in ('SetReceiving','StartCapture','StopCapture') and int(r['phase'])>=5 and not (r['label'].startswith(('rb_dma_','rb_start_b','rb_stop_b')) or r['label']=='cleanup'):errors.append('unknown_work_entry')
    for a,n in [('a102',7 if dense else 11),('a103',3),('a11f',5),('a355',3)]:
        wr=[r for r in calls if r['operation']=='WriteMcuVariableI2c' and r['addr']==a]
        if len(wr)!=n or any(r['result']!='00000000' for r in wr) or (a=='a103' and any(r['value']!='5' for r in wr)):errors.append('MCU_budget:'+a)
    gate=[r for r in calls if r['label']=='rb_gate' and r['operation']=='WriteMcuVariableI2c']
    if [int(r['value']) for r in gate]!=[(saved.get('a102',0)&~15)|v for v in ((0,0) if dense else (0,2,0,0,2,0))]:errors.append('FD_ABA_gate_sequence')
    profile=[r for r in calls if r['operation']=='SetFrameRate' and r['label'] in ('cal_setup','rb_profile15','rb_profile_restore5')]
    if [r['value'] for r in profile]!=['5','0','5'] or any(r['result']!='00000000' for r in profile):errors.append('profile_sequence')
    if len(rec['SEQ_REFRESH'])!=3 or [r.get('restoring') for r in rec['SEQ_REFRESH']]!=['0','0','1'] or any(r.get('verified')!='1' for r in rec['SEQ_REFRESH']):errors.append('Refresh_proof')
    for tag in ('PREVIEW_AWB_RESTORE','SEQ_RESTORE','AWB_RESTORE'):
        rs=rec[tag];mask=96 if tag=='AWB_RESTORE' else 65535
        if len(rs)!=1 or rs[0].get('verified')!='1' or (int(rs[0].get('saved','-1'))&mask)!=(int(rs[0].get('actual','-2'))&mask):errors.append('restore:'+tag)
    for original,restored,name in [('sweep_original','sweep_restore_controls','calibration_saved'),('sweep_ref_00','sweep_restore_isp','isp_gain')]:
        if name=='calibration_saved':a=ri[original,'-1','saved',name];z=ri[restored,'-1','saved',name];count=5
        else:a=[r for r in reads if r['label']==original and r['when']=='isp_pin'];z=[r for r in reads if r['label']==restored and r['when']=='verified'];count=4
        if len(a)!=count or len(z)!=count or any(r['result']!='00000000' for r in a+z) or {r['addr']:r['value'] for r in a}!={r['addr']:r['value'] for r in z}:errors.append('physical_restore:'+restored)
    pattern=re.compile(r'BOOT run=(\d+) seq=(\d+) stage=(\d+) addr=([0-9a-f]{8}) result=([0-9a-f]{8}) at=(\d+) heap=(\d+) linear=(\d+)');startup=[]
    for line in (p/'startup_journal.txt').read_text().splitlines():
        m=pattern.fullmatch(line)
        if not m:errors.append('boot_format');continue
        run,seq,stage,addr,result,at,heap,linear=m.groups();startup.append(dict(run=int(run),seq=int(seq),stage=int(stage),address=int(addr,16),result=result,at=int(at),heap=int(heap),linear=int(linear)))
    stages=[r['stage'] for r in startup]
    if len(startup)!=204 or [r['seq'] for r in startup]!=list(range(len(startup))) or len({r['run'] for r in startup})!=1 or stages[:12]!=list(range(1,13)) or stages[-4:]!=[19,20,21,22] or any(r['stage'] not in BOOT_STAGES or r['stage'] in (25,255) or r['result']!='00000000' or r['heap']<=0 or r['linear']<=0 for r in startup):errors.append('boot_lifecycle')
    if [r['at'] for r in startup]!=sorted(r['at'] for r in startup):errors.append('boot_time')
    for stage,n in ((13,2),(14,2),(15,4),(16,88),(17,88),(18,4)):
        if stages.count(stage)!=n:errors.append('boot_count:'+str(stage))
    br=[r for r in reads if r['name']=='ccm_byte'];paired=[r for r in startup if r['stage'] in (16,17)]
    if len(paired)!=len(br)*2:errors.append('boot_byte_count')
    else:
        for i,row in enumerate(br):
            a,z=paired[2*i:2*i+2]
            if a['stage']!=16 or z['stage']!=17 or a['address']!=int(row['addr'],16) or z['address']!=a['address'] or not a['at']<=int(row['time_ms'])<=z['at']:errors.append('boot_byte_boundary')
    if rec['REBUILD_END']!=[dict(complete='1',points='74',DMA=str(total_DMA),restored='1')]:errors.append('rebuild_incomplete')
    if len(rec['END'])!=1 or any(rec['END'][0].get(k)!=v for k,v in dict(complete='1',aborted='0',io_error='0').items()):errors.append('run_incomplete')
    return dict(schema='camera-exposure-dense-report/v1' if dense else 'camera-exposure-rebuild-report/v2',source=expected_source,passed=not errors,calibration_validated=False,optical_time_verified=False,writer_identified=False,files_verified=len(receipts),points=len(points),DMA_frames=len(frames),raw_target_images=len(raw_names),lighting_assumed_steady=False,Y2R_matrix=matrix_value,blocks=blocks,errors=errors,startup_journal=startup)

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('directory',type=Path);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();r=analyse(a.directory)
    with a.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps(dict(passed=r['passed'],points=r['points'],DMA=r['DMA_frames'],images=r['raw_target_images'],errors=r['errors'])))
    raise SystemExit(0 if r['passed'] else 2)
