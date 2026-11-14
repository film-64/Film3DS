#!/usr/bin/env python3
"""Dense independent exposure fit with fixed alternating holdout."""
from exposure_response_fit import analyse as _analyse
from exposure_sweep_contract import EXPOSURE_DENSE_RESPONSE_SOURCE

def analyse(directory):return _analyse(directory,EXPOSURE_DENSE_RESPONSE_SOURCE)

if __name__=='__main__':
    import argparse,json
    from pathlib import Path
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args();r=analyse(a.directory)
    with a.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps([dict(fps=x['fps'],passed=x['relative_response_passed'],reason=x['reason'],effective_training=x.get('effective_training_indices'),holdout=x.get('holdout')) for x in r['profiles']]))
