"""Extract private nine-tone grain residual patches; do not redistribute references."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib
import numpy as np
from PIL import Image
from camera_runtime_contract import CONTRACT_MATRIX
from measure_grain import measure

SOURCE='film-grain-texture-build/v1'
def build(reference,profile,out,source=SOURCE):
    rule=CONTRACT_MATRIX.get(source)
    if source!=SOURCE or not rule or not rule['compute'] or rule['lane_admission'] or rule['override'] is not None:
        raise ValueError('unregistered grain asset source')
    reference,profile,out=map(Path,(reference,profile,out))
    report_path=out.with_suffix('.json')
    if out.exists() or report_path.exists():raise FileExistsError(out)
    records=profile.read_text().split();assert records[0]=='F3G1' and len(records)==28
    bands=np.array(list(map(float,records[1:]))).reshape(9,3)
    measured=measure(reference)
    if not np.allclose(bands[:,0],[r['rendered_mean_code'] for r in measured],atol=.001,rtol=0) or not np.allclose(bands[:,1],[r['grain_rms_code'] for r in measured],atol=.001,rtol=0):
        raise ValueError('reference pixels do not match the selected measured grain profile')
    with Image.open(reference) as im:
        assert im.size==(1536,1536)
        if im.mode in ('I','I;16','I;16B','I;16L'):y=np.asarray(im,dtype=float)/257
        else:y=np.asarray(im.convert('RGB'),dtype=float)@np.array([.2126,.7152,.0722])
    side=rule['side'];yy,xx=np.mgrid[:side,:side]
    design=np.stack([np.ones(side*side),xx.ravel(),yy.ravel()],-1)
    tiles=[];stats=[]
    for i in range(9):
        x=(i%3)*512+192;y0=(i//3)*512+192
        patch=y[y0:y0+side,x:x+side]
        plane=(design@np.linalg.lstsq(design,patch.ravel(),rcond=None)[0]).reshape(side,side)
        residual=patch-plane;sigma=float(residual.std())
        if not sigma>0:raise ValueError('flat reference has no grain')
        normalized=residual/sigma
        if np.max(np.abs(normalized))>=7.99:raise ValueError('reference residual exceeds lossless atlas range')
        tile=np.rint(normalized*4096).astype('<i2');tiles.append(tile)
        stats.append(dict(code=float(bands[i,0]),source_sigma=sigma,normalized_mean=float(tile.mean()/4096),normalized_rms=float(np.sqrt(np.mean((tile.astype(float)/4096)**2)))))
    payload=struct.pack('<4s4I9f',b'F3GT',1,side,side,9,*bands[:,0])+np.stack(tiles).tobytes()
    payload+=struct.pack('<I',zlib.crc32(payload))
    out.parent.mkdir(parents=True,exist_ok=True)
    with out.open('xb') as f:f.write(payload)
    report=dict(source=source,local_trace=dict(source=source),
        reference=str(reference),reference_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
        profile_sha256=hashlib.sha256(profile.read_bytes()).hexdigest(),atlas_sha256=hashlib.sha256(payload).hexdigest(),
        format='F3GT/v1',side=side,bands=stats,scope='private derived texture patches; not a new film-stock calibration')
    with report_path.open('x') as f:json.dump(report,f,indent=2);f.write('\n')
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True);p.add_argument('--profile',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();r=build(a.reference,a.profile,a.out);print(json.dumps(dict(format=r['format'],bands=9,side=r['side'],sha256=r['atlas_sha256'])))
