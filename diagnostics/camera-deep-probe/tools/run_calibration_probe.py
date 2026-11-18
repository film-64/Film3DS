#!/usr/bin/env python3
"""One calibration deployment, then bounded readiness polling and PC-initiated recovery."""
import argparse,hashlib,http.client,json,time
from pathlib import Path
from run_deep_probe import deploy
from retrieve_probe_archive import retrieve

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('ip');p.add_argument('binary',type=Path)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--sensor',type=int,choices=(1,2,3),default=1)
    p.add_argument('--receive-only',action='store_true');p.add_argument('--smoke',action='store_true');args=p.parse_args()
    binary_hash=hashlib.sha256(args.binary.read_bytes()).hexdigest()
    root=Path(__file__).resolve().parents[1]
    if args.receive_only:
        previous=json.loads((args.out/'deployment.json').read_text())
        if previous['binary_sha256']!=binary_hash or previous['device']!=args.ip:raise ValueError('deployment provenance changed')
    else:
        args.out.mkdir(parents=True,exist_ok=False)
        files=[*sorted((root/'probes/exposure').glob('*')),root/'probes/recovery/main.c',root/'Makefile.probe']
        hashes={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files if f.is_file()}
        record={'device':args.ip,'binary_sha256':binary_hash,'source_sha256':hashes,
                'request':['--auto','--calibrate',f'--sensor={args.sensor}']+(['--smoke'] if args.smoke else []),'contract':'calibration-1',
                'expected_stages':1 if args.smoke else 58,'transfer':'device SD gzip, PC-initiated HTTP ranges',
                'readiness_poll_seconds':30,'readiness_deadline_seconds':7200,'started_unix':time.time()}
        (args.out/'deployment.json').write_text(json.dumps(record,indent=2)+'\n')
        archive=args.out/'deployed-source';archive.mkdir()
        import shutil
        for f in files:
            if f.is_file():
                target=archive/f.relative_to(root);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,target)
        shutil.copyfile(args.binary,args.out/args.binary.name)
        deploy(args.ip,args.binary,arguments=tuple(record['request']))
    print('Waiting for calibration and SD compression; polling HTTP 5000 every 30s for at most 2h. No redeployment or measurement retries.',flush=True)
    deadline=time.monotonic()+7200
    while time.monotonic()<deadline:
        c=http.client.HTTPConnection(args.ip,5000,timeout=5)
        try:
            c.request('GET','/info');r=c.getresponse();data=r.read(1024)
            if r.status==200 and data.startswith(b'FILMRECOVERY 1\n'):break
        except (OSError,http.client.HTTPException):pass
        finally:c.close()
        time.sleep(30)
    else:raise TimeoutError('recovery service not ready within explicit 2h window; SD originals retained')
    print('Archive service ready; downloading and checking native CRCs',flush=True)
    retrieve(args.ip,args.out/'recovered')
    print('CALIBRATION FILES RECOVERED; measurement completeness still requires calibration_report.py',flush=True)

if __name__=='__main__':main()
