#!/usr/bin/env python3
"""CCM v3: strict byte/reference evidence plus bounded startup journal."""
import argparse
import json
from pathlib import Path
from camera_hg_report import analyse as shared_analyse
from exposure_sweep_contract import CCM_BOOT_SOURCE

def analyse(directory):return shared_analyse(directory,source=CCM_BOOT_SOURCE)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();r=analyse(a.directory)
    with a.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps(dict(passed=r['passed'],conclusion=r['conclusion'],errors=r['errors'],files=r['files_verified'])))
    raise SystemExit(0 if r['passed'] else 2)
