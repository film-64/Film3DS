#!/usr/bin/env python3
"""Strict dense capture verifier; old reconstruction sources cannot masquerade."""
from exposure_rebuild_report import analyse as _analyse
from exposure_sweep_contract import EXPOSURE_DENSE_SOURCE

def analyse(directory):return _analyse(directory,EXPOSURE_DENSE_SOURCE)

if __name__=='__main__':
    import argparse,json
    from pathlib import Path
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args();r=analyse(a.directory)
    with a.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps(dict(passed=r['passed'],points=r['points'],DMA=r['DMA_frames'],images=r['raw_target_images'],errors=r['errors'])))
    raise SystemExit(0 if r['passed'] else 2)
