"""Frozen host/3DS scene probe, one official deployment and existing RAM recovery."""
import argparse,json,subprocess,uuid
from pathlib import Path
import numpy as np
from camera_ui15_package import checked_manifest,sha
from camera_runtime_contract import CONTRACT_MATRIX
from retrieve_probe_ram import retrieve,parse_index,get
ROOT=Path(__file__).resolve().parents[1]

def compare(reference,device):
    a=np.fromfile(reference/'canvas.rgb16',dtype='<u2').astype(int)
    b=np.fromfile(device/'canvas.rgb16',dtype='<u2').astype(int)
    if a.shape!=b.shape or not a.size:raise ValueError('canvas dimensions differ')
    error=np.abs(a-b)
    x=np.fromfile(reference/'samples.f32',dtype='<f4');y=np.fromfile(device/'samples.f32',dtype='<f4')
    if x.shape!=y.shape or not np.isfinite(y).all():raise ValueError('invalid scene samples')
    result=dict(canvas_max_code_error=int(error.max()),canvas_mean_code_error=float(error.mean()),
        scene_max_absolute_error=float(np.abs(x-y).max()),
        passed=bool(error.max()<=4 and np.allclose(x,y,rtol=1e-5,atol=1e-6)),
        tolerances=dict(canvas_max_code_error=4,scene_rtol=1e-5,scene_atol=1e-6))
    (device/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
    if not result['passed']:raise ValueError(str(result))
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=('host','deploy','retrieve','compare'))
    p.add_argument('--out',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--package',type=Path)
    p.add_argument('--case',choices=('color','gray'),default='color');p.add_argument('--ip');p.add_argument('--token')
    p.add_argument('--link',type=Path,default=Path('/Users/a64/Documents/Codex/.tools/film3ds-3dslink/3dslink'))
    a=p.parse_args();out=a.out.resolve()
    if a.action=='host':
        subprocess.run([str(ROOT/'build/filmscene'),str(ROOT/f'romfs/rawqa/{a.case}.F3S'),str(ROOT/f'romfs/calibration/display-{a.case}.f3isp'),str(out),'0','0','0','0','0','.35','1.5'],check=True)
    elif a.action=='deploy':
        if not a.package or not a.ip:p.error('--package and --ip required')
        assert CONTRACT_MATRIX['film-raw-scene-deploy/v1']['retries']==0
        package=a.package.resolve();checked_manifest(package);built=json.loads((package/'build.json').read_text())
        if built!=dict(app_sha256=sha(package/'Film3DS.3dsx'),source_manifest_sha256=sha(package/'package.json'),arm11_verified=True,romfs_verified=True):raise ValueError('unverified frozen binary')
        out.mkdir();token=uuid.uuid4().hex
        record=dict(source='film-raw-scene-deploy/v1',token=token,ip=a.ip,case=a.case,app_sha256=built['app_sha256'],status='started')
        receipt=out/'deployment.json';receipt.write_text(json.dumps(record,indent=2)+'\n')
        command=[str(a.link),'-a',a.ip,'-r','1',str(package/'Film3DS.3dsx'),f'f3sceneqa={token}']
        if a.case=='gray':command.append('f3scene-gray')
        try:
            r=subprocess.run(command,capture_output=True,text=True,timeout=150)
            (out/'3dslink.log').write_text(r.stdout+r.stderr)
            record.update(returncode=r.returncode,status='launch_requested' if r.returncode==0 else 'failed')
        except (OSError,subprocess.TimeoutExpired) as e:
            record.update(status='failed',error=str(e));receipt.write_text(json.dumps(record,indent=2)+'\n');raise
        receipt.write_text(json.dumps(record,indent=2)+'\n')
        print(json.dumps(record))
        if r.returncode:raise SystemExit(r.returncode)
    elif a.action=='retrieve':
        if not a.ip or not a.token or len(a.token)!=32 or any(c not in '0123456789abcdef' for c in a.token):p.error('--ip and exact deployment --token required')
        status,_,data=get(a.ip,'/info');prefix,_=parse_index(data)
        if status!=200 or prefix['run']!=a.token or prefix['complete']!='1' or prefix['failed']!='0':raise ValueError('probe token/completion mismatch')
        retrieve(a.ip,out)
        if a.reference:print(compare(a.reference.resolve(),out))
    else:
        if not a.reference:p.error('--reference required')
        print(compare(a.reference.resolve(),out))
if __name__=='__main__':main()
