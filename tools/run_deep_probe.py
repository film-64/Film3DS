#!/usr/bin/env python3
"""Upload the enhanced FilmProbe and retain each acknowledged evidence checkpoint."""
import argparse,hashlib,json,socket,struct,time,zlib
from pathlib import Path

def exact(stream,size):
    data=bytearray()
    while len(data)<size:
        part=stream.read(size-len(data))
        if not part:raise EOFError('incomplete network payload')
        data.extend(part)
    return bytes(data)

def deploy(ip,binary,arguments=('--auto','--direct')):
    payload=binary.read_bytes();name=binary.name.encode('ascii')
    with socket.create_connection((ip,17491),timeout=10) as sock:
        sock.settimeout(45);stream=sock.makefile('rb')
        print('NetLoader connected:',sock.getsockname(),flush=True)
        sock.sendall(struct.pack('<I',len(name))+name+struct.pack('<I',len(payload)))
        accepted=struct.unpack('<i',exact(stream,4))[0]
        if accepted:raise RuntimeError(f'NetLoader rejected binary: {accepted}')
        compressed=zlib.compress(payload)
        for pos in range(0,len(compressed),16384):
            chunk=compressed[pos:pos+16384];sock.sendall(struct.pack('<I',len(chunk))+chunk)
        accepted=struct.unpack('<i',exact(stream,4))[0]
        if accepted:raise RuntimeError(f'NetLoader rejected payload: {accepted}')
        args=b'sdmc:/3ds/'+name+b'\0'+b''.join(a.encode('ascii')+b'\0' for a in arguments)
        sock.sendall(struct.pack('<I',len(args))+args)
    print('Deployed',len(payload),'bytes',flush=True)

def receive(conn,root):
    stream=conn.makefile('rb');line=stream.readline(128)
    parts=line.decode('ascii').strip().split()
    if len(parts)!=3 or parts[:2]!=['FILMPROBE_FILES','2']:raise ValueError('invalid greeting')
    tag=parts[2]
    if tag not in {'startup','drivers','sensor-1','sensor-2','sensor-3','final'}:raise ValueError('invalid checkpoint')
    directory=root/tag;directory.mkdir(exist_ok=False)
    files=[]
    while True:
        line=stream.readline(1024)
        if line==b'DONE\n':
            (directory/'received-sha256.json').write_text(json.dumps(files,indent=2)+'\n')
            conn.sendall(b'K');print('CHECKPOINT COMPLETE',tag,len(files),'files',flush=True)
            return tag
        parts=line.decode('ascii').strip().split()
        if len(parts)!=3 or parts[0]!='FILE':raise ValueError(repr(line))
        name,size=parts[1],int(parts[2])
        limit=512*1024*1024 if name=='ipc.csv' else 100_000_000
        if Path(name).name!=name or name in {'.','..'} or not 0<=size<=limit:raise ValueError('invalid file')
        digest=hashlib.sha256();remaining=size
        with (directory/name).open('xb') as f:
            while remaining:
                chunk=stream.read(min(65536,remaining))
                if not chunk:raise EOFError(name)
                f.write(chunk);digest.update(chunk);remaining-=len(chunk)
        files.append({'name':name,'bytes':size,'sha256':digest.hexdigest()})
        print('Received',tag,name,size,flush=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ip');parser.add_argument('binary',type=Path);parser.add_argument('--out',required=True,type=Path)
    parser.add_argument('--sensor',type=int,choices=(1,2,3))
    parser.add_argument('--from-phase',type=int,default=3)
    parser.add_argument('--receive-only',action='store_true',help='Continue listening for an already-running probe; do not deploy')
    args=parser.parse_args()
    if not 3 <= args.from_phase <= 58 or (args.from_phase>3 and args.sensor is None):
        parser.error('--from-phase must be 3..58 and resuming requires --sensor')
    if args.receive_only:
        previous=json.loads((args.out/'deployment.json').read_text())
        if previous['device']!=args.ip or previous['binary_sha256']!=hashlib.sha256(args.binary.read_bytes()).hexdigest():
            raise ValueError('receive-only provenance mismatch')
        (args.out/f'receiver-resumed-{time.time_ns()}.json').write_text(json.dumps({'resumed_unix':time.time(),'ipc_file_limit':512*1024*1024})+'\n')
    else:
        args.out.mkdir(parents=True,exist_ok=False)
    source=Path(__file__).resolve().parents[1]
    hashes={str(p.relative_to(source)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted((source/'probes/exposure').glob('*')) if p.is_file()}
    if not args.receive_only:(args.out/'deployment.json').write_text(json.dumps({'device':args.ip,'base_revision':'5a1e62d9d7436c594776716fdbd723855209c53d','probe_revision':'5a1e62d-deep5','request':{'only_sensor':args.sensor or 0,'first_phase':args.from_phase,'last_phase':72},'transfer_policy':'final_only','binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'source_sha256':hashes,'started_unix':time.time()},indent=2)+'\n')
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        server.bind(('0.0.0.0',17491));server.listen(1);server.settimeout(7200)
        if not args.receive_only:
            arguments=['--auto','--direct']
            if args.sensor is not None:arguments.append(f'--sensor={args.sensor}')
            if args.from_phase!=3:arguments.append(f'--from-phase={args.from_phase}')
            deploy(args.ip,args.binary,arguments=tuple(arguments))
        while True:
            print('Waiting for next evidence checkpoint',flush=True)
            conn,peer=server.accept()
            with conn:
                if peer[0]!=args.ip:raise ValueError(f'unexpected sender {peer}')
                conn.settimeout(45)
                if receive(conn,args.out)=='final':break
    print('FINAL EVIDENCE RECEIVED',args.out,flush=True)
if __name__=='__main__':main()
