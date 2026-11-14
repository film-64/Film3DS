#!/usr/bin/env python3
"""Fixed-partition relative response analysis of strictly verified camera evidence."""
import argparse,hashlib,json,math,sys,warnings
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT.parent/'.tools/exposure-calibration'))
import numpy as np
from scipy.optimize import lsq_linear
import scipy
from exposure_sweep_contract import CONTRACT_MATRIX,EXPOSURE_RESPONSE_SOURCE,EXPOSURE_REBUILD_SOURCE,EXPOSURE_DENSE_RESPONSE_SOURCE,EXPOSURE_DENSE_SOURCE
from exposure_rebuild_report import analyse as verify_rebuild
RULE=CONTRACT_MATRIX[EXPOSURE_RESPONSE_SOURCE]


def fit_profile(images,times,source=EXPOSURE_RESPONSE_SOURCE):
    if source not in (EXPOSURE_RESPONSE_SOURCE,EXPOSURE_DENSE_RESPONSE_SOURCE):raise ValueError('unregistered response analysis source')
    RULE=CONTRACT_MATRIX[source];dense=source==EXPOSURE_DENSE_RESPONSE_SOURCE
    n=len(images);training=list(range(0,n,2)) if dense else [i for i in RULE['training_indices'] if i<n];holdout=list(range(1,n,2)) if dense else [i for i in RULE['holdout_indices'] if i<n]
    if len(times)!=n or n not in ((15,19) if dense else (8,10)) or any(not math.isfinite(float(t)) or t<=0 for t in times) or any(b<=a for a,b in zip(times,times[1:])):raise ValueError('unregistered or invalid exposure grid')
    if any(im.dtype!=np.uint8 or im.shape!=images[0].shape or im.ndim!=3 or im.shape[2]!=3 for im in images):raise ValueError('invalid RGB image grid')
    lo,hi=RULE['encoded_domain'];valid=[(im>=lo)&(im<=hi) for im in images];plateau=[]
    for a,b in zip(training,training[1:]):
        ev=math.log2(times[b]/times[a])
        for c in range(3):
            za=float(np.median(images[a][...,c]));zb=float(np.median(images[b][...,c]));slope=(zb-za)/ev
            if za>=RULE['training_tail_plateau']['upper_training_median_at_least'] and slope<RULE['training_tail_plateau']['maximum_code_rise_per_EV']:
                valid[b][...,c]=False
                if dense:valid[a][...,c]=False
                plateau.append(dict(index=b,channel=c,lower_index=a,lower_median=za,upper_median=zb,code_rise_per_EV=slope))
    effective=[[i for i in training if float(valid[i][...,c].mean())>=RULE['training_exposure_coverage']] for c in range(3)]
    result=dict(training_indices=training,holdout_indices=holdout,effective_training_indices=effective,training_plateau_masks=plateau,relative_response_passed=False,calibration_validated=False,optical_time_verified=False)
    if any(len(v)<RULE['minimum_training_exposures'] for v in effective):return dict(**result,reason='insufficient_effective_training_exposures',required=RULE['minimum_training_exposures'])
    domains=[]
    for c in range(3):
        z=np.concatenate([images[i][...,c][valid[i][...,c]] for i in training]);low,high=int(z.min()),int(z.max());domains.append([low,high])
        if high-low<RULE['minimum_domain_span']:return dict(**result,reason='insufficient_training_code_span',domain=domains)
    h,w,_=images[0].shape;ys=np.linspace(0,h-1,10,dtype=int);xs=np.linspace(0,w-1,10,dtype=int)
    cumulative=(np.arange(255)[None,:]<np.arange(256)[:,None]).astype(float)
    smooth=np.diff(np.eye(255),axis=0);weight=np.minimum(np.arange(1,255),255-np.arange(1,255));regularizer=smooth*(RULE['solver']['smoothing']*weight[:,None])
    curves=[];solver_info=[]
    for c in range(3):
        z=np.stack([images[i][np.ix_(ys,xs)][...,c].reshape(-1) for i in training]);masks=np.stack([valid[i][np.ix_(ys,xs)][...,c].reshape(-1) for i in training]);logt=np.log(np.asarray([times[i] for i in training]));matrices=[];targets=[]
        for pixel in range(z.shape[1]):
            mask=masks[:,pixel]
            if mask.sum()<2:continue
            code=z[mask,pixel].astype(int);t=logt[mask];weights=np.minimum(code,255-code).astype(float);design=cumulative[code]
            matrices.append((design-np.average(design,axis=0,weights=weights**2))*weights[:,None]);targets.append((t-np.average(t,weights=weights**2))*weights)
        if len(matrices)<20:return dict(**result,reason='insufficient_paired_training_pixels',domain=domains)
        solved=lsq_linear(np.vstack(matrices+[regularizer]),np.concatenate(targets+[np.zeros(254)]),bounds=(RULE['solver']['min_log_increment'],np.inf),method='bvls',tol=RULE['solver']['tol'],max_iter=RULE['solver']['iterations'])
        solver_info.append(dict(channel=c,success=bool(solved.success),iterations=int(solved.nit),cost=float(solved.cost)))
        if not solved.success or not np.isfinite(solved.x).all():return dict(**result,reason='solver_failure',domain=domains,solver=solver_info)
        curve=np.r_[0,np.cumsum(solved.x)];curve-=curve[128];curves.append(curve/math.log(2))
    response=np.stack(curves,axis=1)
    def invert(im,t):
        mask=np.ones(im.shape[:2],bool);log=np.empty(im.shape,float)
        for c,(low,high) in enumerate(domains):mask &= (im[...,c]>=low)&(im[...,c]<=high);log[...,c]=response[im[...,c],c]-math.log2(t)
        return log,mask
    train_logs=[]
    for i in training:
        log,mask=invert(images[i],times[i]);channel_mask=valid[i]&mask[...,None];train_logs.append(np.where(channel_mask,log,np.nan))
    stack=np.stack(train_logs);counts=np.isfinite(stack).sum(axis=0)
    with warnings.catch_warnings():warnings.simplefilter('ignore',RuntimeWarning);baseline=np.nanmedian(stack,axis=0)
    checks=[]
    for i in holdout:
        log,mask=invert(images[i],times[i]);mask &= (counts>=2).all(axis=2);fraction=float(mask.mean());error=np.median(log[mask]-baseline[mask],axis=0) if fraction>=RULE['holdout_pixel_coverage'] else None
        checks.append(dict(index=i,common_pixel_fraction=fraction,channel_error_EV=error.tolist() if error is not None else None,passed=error is not None and bool((np.abs(error)<=RULE['max_holdout_error_EV']).all())))
    usable=[x for x in checks if x['channel_error_EV'] is not None];passed=len(usable)>=RULE['minimum_holdout_exposures'] and all(x['passed'] for x in usable)
    return dict(**{k:v for k,v in result.items() if k!='relative_response_passed'},relative_response_passed=passed,reason='holdout_pass' if passed else 'insufficient_or_failed_holdout',domain=domains,log2_response=response.tolist(),solver=solver_info,holdout=checks,_baseline=baseline,_counts=counts)


def image_rgb(path,matrix):
    data=path.read_bytes()
    if len(data)!=24576:raise ValueError('invalid ROI size')
    q=np.frombuffer(data,np.uint8).reshape(96,64,4).astype(float);y=np.empty((96,128));y[:,::2]=q[:,:,0];y[:,1::2]=q[:,:,2];limited,hd=matrix>=2,matrix%2==1
    y=(y-(16 if limited else 0))/(219 if limited else 255);u=np.repeat((q[:,:,1]-128)/(224 if limited else 255),2,axis=1);v=np.repeat((q[:,:,3]-128)/(224 if limited else 255),2,axis=1)
    rgb=np.stack([y+(1.5748 if hd else 1.402)*v,y-(.187324 if hd else .344136)*u-(.468124 if hd else .714136)*v,y+(1.8556 if hd else 1.772)*u],axis=-1)
    return np.rint(np.clip(rgb,0,1)*255).astype(np.uint8)


def analyse(directory,source=EXPOSURE_RESPONSE_SOURCE):
    if source not in (EXPOSURE_RESPONSE_SOURCE,EXPOSURE_DENSE_RESPONSE_SOURCE):raise ValueError('unregistered response analysis source')
    RULE=CONTRACT_MATRIX[source];dense=source==EXPOSURE_DENSE_RESPONSE_SOURCE
    p=Path(directory);verified=verify_rebuild(p,EXPOSURE_DENSE_SOURCE if dense else EXPOSURE_REBUILD_SOURCE)
    if not verified['passed'] or verified['source']!=RULE['input_source']:raise ValueError('unverified or unknown capture source')
    profiles=[]
    for block in verified['blocks']:
        pts=[x for x in block['points'] if x['kind']=='coarse'];images=[image_rgb(p/x['images'][-1],verified['Y2R_matrix']) for x in pts];times=[x['estimated_seconds'] for x in pts]
        if not block['timing_estimate_usable'] or any(t is None for t in times):profiles.append(dict(fps=block['fps'],relative_response_passed=False,reason='timing_estimate_unavailable'));continue
        fit=fit_profile(images,times,source);baseline=fit.pop('_baseline',None);counts=fit.pop('_counts',None)
        repeat_checks=[];gain_rows=[]
        if 'log2_response' in fit:
            response=np.array(fit['log2_response']);domains=fit['domain']
            def error(im,t):
                mask=(counts>=2).all(axis=2);log=np.empty(im.shape,float)
                for c,(low,high) in enumerate(domains):mask &= (im[...,c]>=low)&(im[...,c]<=high);log[...,c]=response[im[...,c],c]-math.log2(t)
                fraction=float(mask.mean());value=np.median(log[mask]-baseline[mask],axis=0).tolist() if fraction>=RULE['holdout_pixel_coverage'] else None
                return fraction,value
            for check in fit['holdout']:
                pt=pts[check['index']];im=image_rgb(p/pt['images'][-2],verified['Y2R_matrix']);fraction,value=error(im,pt['estimated_seconds']);repeat_checks.append(dict(index=check['index'],common_pixel_fraction=fraction,channel_error_EV=value,passed=value is not None and max(map(abs,value))<=RULE['max_holdout_error_EV']))
            usable_repeat=[x for x in repeat_checks if x['channel_error_EV'] is not None]
            fit['relative_response_passed']=fit['relative_response_passed'] and len(usable_repeat)>=RULE['minimum_holdout_exposures'] and all(x['passed'] for x in usable_repeat)
            fit['second_image_holdout']=repeat_checks
            if fit['relative_response_passed']:
                for pt in block['points']:
                    if pt['kind']!='gain':continue
                    checks=[error(image_rgb(p/name,verified['Y2R_matrix']),pt['estimated_seconds']) for name in pt['images'][-2:]];values=[x[1] for x in checks]
                    gain_rows.append(dict(point=pt['index'],coarse=pt['coarse'],core_code=pt['core'],ISP=pt['ISP_target'][0],coverage=min(x[0] for x in checks),effective_RGB_shift_EV=np.mean(values,axis=0).tolist() if all(v is not None for v in values) else None))
        transfer=[]
        for core in (528,544,1040):
            for isp in (96,128,160):
                rows=[x for x in gain_rows if x['core_code']==core and x['ISP']==isp]
                usable=len(rows)==2 and len({x['coarse'] for x in rows})==2 and all(x['effective_RGB_shift_EV'] is not None for x in rows)
                difference=(np.array(rows[0]['effective_RGB_shift_EV'])-np.array(rows[1]['effective_RGB_shift_EV'])).tolist() if usable else None
                transfer.append(dict(core_code=core,ISP=isp,two_distinct_valid_anchors=usable,channel_difference_EV=difference,passed=usable and max(map(abs,difference))<=RULE['max_holdout_error_EV']))
        fit['gain_transfer_checks']=transfer
        start=block['snapshots']['before_stream'];end=block['snapshots']['after_stop'];changed={k:[v,end.get(k)] for k,v in start.items() if isinstance(v,int) and end.get(k)!=v};fingerprint={k:v for k,v in start.items() if isinstance(v,int)};fingerprint.update(fps=block['fps'],Y2R=verified['Y2R_matrix'],timing_baseline=block['baseline'])
        fit.update(fps=block['fps'],pipeline_fingerprint=fingerprint,pipeline_fingerprint_sha256=hashlib.sha256(json.dumps(fingerprint,sort_keys=True).encode()).hexdigest(),start_to_post_FD_pipeline_changes=changed,gain_observations=gain_rows)
        profiles.append(fit)
    return dict(schema=source,input_source=RULE['input_source'],manifest_sha256=hashlib.sha256((p/'manifest.txt').read_bytes()).hexdigest(),analysis_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),contract=RULE,profiles=profiles,absolute_calibration_validated=False,optical_time_verified=False,external_reference_used_in_fit=False,dependencies=dict(numpy=np.__version__,scipy=scipy.__version__))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);parser.add_argument('--out',type=Path,required=True);a=parser.parse_args();r=analyse(a.directory)
    with a.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps([dict(fps=x['fps'],passed=x['relative_response_passed'],reason=x['reason'],effective_training=x.get('effective_training_indices'),holdout=x.get('holdout')) for x in r['profiles']]))
