#!/usr/bin/env python3
"""Read-only recovery of an existing SD probe run via ftpd. Never starts a measurement."""
import argparse,ftplib,hashlib,json,time,re
from pathlib import Path

def basename(value):
    name=value.rstrip('/').rsplit('/',1)[-1]
    if name in ('','.', '..') or not re.fullmatch(r'[A-Za-z0-9_.-]+',name):raise ValueError('invalid FTP name')
    return name

def connect(ip,source,port=5000):
    ftp=ftplib.FTP()
    try:
        ftp.connect(ip,port,timeout=30);ftp.login();ftp.voidcmd('TYPE I');ftp.cwd(source)
    except BaseException:
        ftp.close();raise
    return ftp

def segment(ip,source,name,offset,length,target,port=5000):
    """One bounded, read-only REST transfer. Never retries a failed connection."""
    ftp=connect(ip,source,port)
    try:
        with ftp.transfercmd(f'RETR {name}',rest=offset if offset else None) as data:
            remaining=length
            while remaining:
                block=data.recv(min(16384,remaining))
                if not block:raise EOFError(f'truncated FTP segment: {name} at {offset+length-remaining}')
                target.write(block);remaining-=len(block)
    finally:
        # A bounded read can end before EOF. Close this session rather than
        # reusing a control stream with pending 426/226 replies.
        ftp.close()

def persistent_segment(ftp,name,offset,length,target):
    """Bounded RETR with explicit ABOR/NOOP synchronization on the same session."""
    if basename(name)!=name:raise ValueError('invalid FTP name')
    payload=bytearray()
    with ftp.transfercmd(f'RETR {name}',rest=offset if offset else None) as data:
        while len(payload)<length:
            block=data.recv(min(16384,length-len(payload)))
            if not block:raise EOFError('truncated persistent segment')
            payload.extend(block)
    ftp.putcmd('ABOR');ftp.putcmd('NOOP')
    for _ in range(6):
        try:response=ftp.getresp()
        except ftplib.error_temp as error:
            response=str(error)
            if response!='425 Transfer aborted' and not response.startswith('426 '):raise
        if response.startswith('200 '):
            target.write(payload);return
        if not response.startswith(('225 ','226 ','425 Transfer aborted','426 ')):
            raise ValueError('unexpected abort synchronization response: '+response)
    raise ValueError('FTP control stream did not synchronize')

def retrieve(ip,out,run=None,chunk_bytes=0,persistent=False):
    if persistent and not chunk_bytes:raise ValueError('persistent mode requires bounded chunk size')
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    with ftplib.FTP() as ftp:
        ftp.connect(ip,5000,timeout=30);ftp.login();ftp.voidcmd('TYPE I')
        ftp.cwd('/3ds/Film3DS/probes')
        if run:
            if basename(run)!=run or not run.startswith('run-'):raise ValueError('run must be one probe directory name')
        else:
            candidates=[]
            for entry in ftp.nlst():
                name=basename(entry)
                if not name.startswith('run-'):continue
                content=[]
                try:ftp.retrbinary(f'RETR {name}/manifest.txt',content.append)
                except ftplib.error_perm:continue
                if b'revision=5a1e62d-deep4' in b''.join(content).split(b'\n',1)[0]:candidates.append(name)
            if not candidates:raise ValueError('no deep4 probe run found')
            run=sorted(candidates)[-1]
        ftp.cwd(run);ftp.voidcmd('TYPE I')
        source=f'/3ds/Film3DS/probes/{run}'
        source_file=out/'recovery-source.json'
        if source_file.exists():
            previous=json.loads(source_file.read_text())
            if previous['device']!=ip or previous['source']!=source:raise ValueError('recovery source changed')
        source_file.write_text(json.dumps({'device':ip,'source':source,'method':'FTP read-only','chunk_bytes':chunk_bytes,'persistent_control':persistent,'started_unix':time.time()},indent=2)+'\n')
        inventory=[]
        for entry in ftp.nlst():
            name=basename(entry)
            if not name.endswith(('.csv','.txt','.pgm','.yuyv','.dmp','.dump')):continue
            size=ftp.size(name)
            limit=512*1024*1024 if name=='ipc.csv' else 100_000_000
            if size is None or not 0<=size<=limit:raise ValueError(f'invalid size: {name}')
            inventory.append((name,size))
        inventory.sort(key=lambda item:({'manifest.txt':0,'frames.csv':1,'calls.csv':2,'reads.csv':3,'ipc.csv':5}.get(item[0],4),item[0]))
        if chunk_bytes and not persistent:ftp.close()
        def fetch(name,start,length,target):
            if persistent:persistent_segment(ftp,name,start,length,target)
            else:segment(ip,source,name,start,length,target)
        files=[]
        for name,size in inventory:
            path=out/name
            if path.exists() and path.stat().st_size==size:
                print('Existing local file',name,size,flush=True)
            else:
                partial=out/(name+'.partial')
                if chunk_bytes:
                    offset=partial.stat().st_size if partial.exists() else 0
                    if offset>size:raise ValueError(f'oversized partial: {name}')
                    # The native run is closed. Check both ends of an existing
                    # prefix before trusting its REST offset after reconnecting.
                    if offset:
                        import io
                        with partial.open('rb') as prior:
                            for start in sorted({0,max(0,offset-4096)}):
                                n=min(4096,offset-start);check=io.BytesIO()
                                fetch(name,start,n,check)
                                prior.seek(start)
                                if check.getvalue()!=prior.read(n):raise ValueError(f'changed remote prefix: {name}')
                        print('Resuming',name,offset,'of',size,flush=True)
                    with partial.open('ab') as f:
                        while offset<size:
                            count=min(chunk_bytes,size-offset)
                            fetch(name,offset,count,f)
                            offset+=count;f.flush()
                            print('Segment',name,offset,'/',size,flush=True)
                else:
                    with partial.open('wb') as f:ftp.retrbinary(f'RETR {name}',f.write,blocksize=65536)
                if partial.stat().st_size!=size:raise ValueError(f'incomplete transfer: {name}')
                partial.replace(path)
                print('Recovered',name,size,flush=True)
            files.append({'name':name,'bytes':size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
            (out/'received-sha256.partial.json').write_text(json.dumps(files,indent=2)+'\n')
        (out/'received-sha256.partial.json').replace(out/'received-sha256.json')
    print('SD RECOVERY COMPLETE',source,len(files),'files',flush=True)
    return source

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('ip')
    parser.add_argument('--out',required=True,type=Path);parser.add_argument('--run')
    parser.add_argument('--chunk-bytes',type=int,default=0,help='bounded REST transfers; retain partial bytes on failure; no automatic retries')
    parser.add_argument('--persistent',action='store_true',help='keep one control session; synchronize every bounded RETR with ABOR and NOOP')
    args=parser.parse_args()
    if args.chunk_bytes and not 4096<=args.chunk_bytes<=1048576:parser.error('chunk bytes must be 4096..1048576')
    if args.persistent and not args.chunk_bytes:parser.error('--persistent requires --chunk-bytes')
    retrieve(args.ip,args.out,args.run,args.chunk_bytes,args.persistent)
if __name__=='__main__':main()
