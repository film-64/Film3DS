"""Reproduce finite offline C RAW/LUT/grain comparisons from verified captures."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import zlib
import numpy as np
from PIL import Image,ImageDraw
from camera_runtime_contract import CONTRACT_MATRIX

ROOT=Path(__file__).resolve().parents[1]
SOURCE='film-raw-darkroom-comparison/v1'
BASE=dict(width=640,height=480,cfa=0,black=[0,0,0,0],white=1023,
          wb=[1.15719418,1,1.34631099],ccm=[1,0,0,0,1,0,0,0,1],
          exposure_ev=0,shoulder=.75,denoise=.5,highlights=0,midtones=0,shadows=0,
          saturation=0,sharpness=1,grain_amount=.35,grain_size=1,grain_coarse=.5,
          grain_shadow=.8,grain_mid=1.1,grain_highlight=.7,seed=1616,lut_strength=1)

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def config(path,parameters):
    with path.open('x') as f:
        f.write('F3RAW1\n')
        for key,value in parameters.items():
            f.write(key+' '+' '.join(str(x) for x in (value if isinstance(value,list) else [value]))+'\n')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--lut',type=Path,required=True);p.add_argument('--atlas',type=Path);a=p.parse_args()
    rule=CONTRACT_MATRIX[SOURCE]
    assert rule['compute'] and not rule['lane_admission'] and rule['override'] is None and rule['retries']==0
    receipts=json.loads((a.run/'recovered/received-ram-sha256.json').read_text())
    proof=json.loads((a.run/'report.json').read_text());assert proof['integrity_verified'] and 0 in proof['complete_cases']
    inputs=[]
    for slot in (0,1):
        path=a.run/f'recovered/c00-raw-{slot:02d}.bin';b=path.read_bytes()
        r=next(r for r in receipts if r['name']==path.name)
        assert len(b)==r['bytes']==640*480*2 and sha(path)==r['sha256'] and f'{zlib.crc32(b)&0xffffffff:08x}'==r['crc32']
        inputs.append(path)
    plan=[('standard',1,{},'medium'),('second-frame',0,{},'medium'),
          ('exposure-minus1',1,dict(exposure_ev=-1),'medium'),('exposure-plus1',1,dict(exposure_ev=1),'medium'),
          ('wb-unity',1,dict(wb=[1,1,1]),'medium'),
          ('fine-grain',1,dict(grain_size=.7,grain_coarse=.2),'fine'),
          ('coarse-grain',1,dict(grain_size=1.6,grain_coarse=.8),'coarse'),
          ('color-detail',1,dict(saturation=.25,sharpness=2,shadows=.2),'medium')]
    assert len(plan)+1<=rule['max_runs']
    a.out.mkdir(parents=True,exist_ok=False)
    frozen_binary=a.out/'filmraw-host';shutil.copy2(ROOT/'build/filmraw',frozen_binary)
    hashes={str(p):sha(p) for p in inputs+[a.lut,a.lut.with_suffix('.f3p'),ROOT/'build/filmraw']}
    for name in ('fine','medium','coarse'):hashes[str(ROOT/f'romfs/grain/{name}.f3g')]=sha(ROOT/f'romfs/grain/{name}.f3g')
    if a.atlas:hashes[str(a.atlas)]=sha(a.atlas)
    report=dict(source=SOURCE,local_trace=dict(source=SOURCE,input_hashes=hashes),plan=plan,
        limits=dict(calibration_certified=False,CFA='RGGB_candidate',black_white_calibrated=False,CCM_calibrated=False,
                    WB='scene_estimate_from_previous_pair',permanent_bad_pixel_map=False,GPU_verified=False),results=[])
    (a.out/'plan.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(name,path,params,grain,with_lut=True):
        parameter_path=a.out/(name+'.params.txt');config(parameter_path,params)
        command=[str(frozen_binary),str(path),str(parameter_path),str(a.out/name),str(ROOT/f'romfs/grain/{grain}.f3g')]
        if with_lut:command.extend([str(a.lut),str(a.lut.with_suffix('.f3p'))])
        textured=bool(a.atlas and grain=='medium')
        if textured:command.append(str(a.atlas))
        r=subprocess.run(command,capture_output=True,text=True)
        record=dict(name=name,command=command,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr,params=params,texture_atlas=textured)
        report['results'].append(record)
        if r.returncode:
            (a.out/'FAILED.json').write_text(json.dumps(report,indent=2)+'\n')
            raise ValueError('C export failed: '+name)
        completed=json.loads((a.out/name/'completed.json').read_text())
        assert completed['source']=='film-raw-darkroom-export/v1' and completed['isp_preparations']==1
        record['outputs']={f.name:sha(f) for f in (a.out/name).iterdir() if f.is_file()}
    for name,slot,changes,grain in plan:run(name,inputs[slot],{**BASE,**changes},grain)
    # Flat RAW-code chart, not a calibrated scene/EV reference.
    chart=np.zeros((288,384),np.uint16);codes=[8,20,40,80,128,256,512,750,1000]
    for i,code in enumerate(codes):chart[(i//3)*96:(i//3+1)*96,(i%3)*128:(i%3+1)*128]=code
    wire=np.stack((chart>>2,chart&3),-1).astype(np.uint8).tobytes()
    chart_path=a.out/'gray-response.bin';chart_path.write_bytes(wire)
    params={**BASE,'width':384,'height':288,'wb':[1,1,1],'shoulder':0,'denoise':0,'sharpness':0,'grain_amount':.5}
    run('gray-response',chart_path,params,'medium',False)
    before=np.asarray(Image.open(a.out/'gray-response/plain.ppm')).astype(float)
    after=np.asarray(Image.open(a.out/'gray-response/plain-grain.ppm')).astype(float)
    stats=[]
    for i,code in enumerate(codes):
        x,y=(i%3)*128,(i//3)*96
        delta=(after-before)[y+24:y+72,x+32:x+96,0]
        stats.append(dict(raw_code=code,display_mean=float(before[y+24:y+72,x+32:x+96,0].mean()),
                          grain_mean_delta=float(delta.mean()),grain_rms=float(np.sqrt((delta**2).mean()))))
    report['grain_response']=stats
    for path,expected in hashes.items():assert sha(Path(path))==expected
    def sheet(filename,items,columns=2):
        w,h=480,360;canvas=Image.new('RGB',(columns*w,((len(items)+columns-1)//columns)*(h+30)),'#202020');d=ImageDraw.Draw(canvas)
        for i,(title,path) in enumerate(items):
            x,y=(i%columns)*w,(i//columns)*(h+30)
            d.text((x+8,y+8),title,fill='white')
            with Image.open(path) as im:canvas.paste(im.resize((w,h),Image.Resampling.LANCZOS),(x,y+28))
        canvas.save(a.out/filename)
    sheet('standard-four-outputs.png',[(title,a.out/'standard'/name) for title,name in
        [('No LUT / no grain','plain.ppm'),('LUT / no grain','look.ppm'),('No LUT / layered grain','plain-grain.ppm'),('LUT / layered grain','look-grain.ppm')]])
    sheet('exposure-comparison.png',[(name,a.out/name/'plain.ppm') for name in ('exposure-minus1','standard','exposure-plus1')],3)
    sheet('grain-comparison.png',[(name,a.out/name/'plain-grain.ppm') for name in ('fine-grain','standard','coarse-grain')],3)
    detail=Image.new('RGB',(960,350),'#202020');d=ImageDraw.Draw(detail)
    for i,name in enumerate(('fine-grain','standard','coarse-grain')):
        with Image.open(a.out/name/'plain-grain.ppm') as im:
            detail.paste(im.crop((240,200,400,360)).resize((320,320),Image.Resampling.NEAREST),(320*i,28))
        d.text((320*i+8,8),name+(' / sample texture' if name=='standard' and a.atlas else ' / procedural'),fill='white')
    detail.save(a.out/'grain-comparison-detail.png')
    (a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(exports=len(report['results']),original_hashes_unchanged=True,grain_response=stats),indent=2))

if __name__=='__main__':main()
