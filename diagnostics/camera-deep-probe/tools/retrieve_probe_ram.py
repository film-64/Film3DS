#!/usr/bin/env python3
"""Read-only bounded HTTP recovery of frozen ram-evidence-1, with per-file CRC32."""
import argparse,hashlib,http.client,json,re,zlib,time
from pathlib import Path
from frozen_transfer_contract import require_policy

def get(ip,path,headers=None,port=5000):
    connection=http.client.HTTPConnection(ip,port,timeout=30)
    try:
        connection.request('GET',path,headers=headers or {})
        response=connection.getresponse();status=response.status;fields=dict(response.getheaders())
        limit=32768 if path=='/info' else 262144
        size=int(response.getheader('Content-Length','-1'))
        if not 0<=size<=limit:raise ValueError('invalid response size')
        data=response.read(size+1)
        if len(data)!=size:raise EOFError('truncated RAM response')
        return status,fields,data
    finally:connection.close()

def parse_index(data):
    lines=data.decode('ascii').splitlines()
    if len(lines)<5 or lines[0] not in ('FILMRAM 1','FILMRAM 2','FILMRAM 3'):raise ValueError('wrong RAM evidence protocol')
    budget=28*1024*1024 if lines[0] in ('FILMRAM 2','FILMRAM 3') else 20*1024*1024
    prefix=dict(line.split(' ',1) for line in lines[1:5])
    if set(prefix)!={'run','complete','failed','count'} or prefix['complete'] not in ('0','1') or prefix['failed'] not in ('0','1'):raise ValueError('invalid RAM status')
    count=int(prefix['count'])
    if not 1<=count<=(320 if lines[0]=='FILMRAM 3' else 40) or len(lines)!=count+5:raise ValueError('invalid RAM file count')
    files=[];seen=set();total=0
    for expected,line in enumerate(lines[5:]):
        ident,name,length,crc=line.split();size=int(length)
        if int(ident)!=expected or name in seen or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,94}',name) or not re.fullmatch(r'[0-9a-f]{8}',crc) or not 0<=size<=20*1024*1024:raise ValueError('invalid RAM file')
        seen.add(name);total+=size;files.append({'id':expected,'name':name,'bytes':size,'crc32':crc})
    if total>budget:raise ValueError('RAM budget exceeded')
    return prefix,files

def retrieve(ip,out,chunk=65536,port=5000,request_gap=0,recovery_policy=None):
    if not 1<=chunk<=262144:raise ValueError('invalid chunk')
    if not 0<=request_gap<=2:raise ValueError('invalid explicit transport pacing')
    rule=require_policy(recovery_policy,chunk) if recovery_policy is not None else None
    transient_failures=0
    status,_,index=get(ip,'/info',port=port)
    if status!=200:raise ValueError('RAM index failed')
    prefix,files=parse_index(index);out=Path(out);out.mkdir(parents=True,exist_ok=True)
    events=out/'transport-events.jsonl'
    if rule and events.exists():
        transient_failures=sum(1 for line in events.read_text().splitlines() if json.loads(line).get('source')=='frozen_ram_http_range')
    fingerprint=hashlib.sha256(index).hexdigest();source=out/'ram-source.json'
    if source.exists() and json.loads(source.read_text())['index_sha256']!=fingerprint:raise ValueError('RAM snapshot changed; existing originals preserved')
    source.write_text(json.dumps({'device':ip,'protocol':'ram-evidence-3' if index.startswith(b'FILMRAM 3\n') else 'ram-evidence-2' if index.startswith(b'FILMRAM 2\n') else 'ram-evidence-1','index_sha256':fingerprint,'status':prefix},indent=2)+'\n');(out/'ram-index.txt').write_bytes(index)
    receipts=[]
    for entry in files:
        target=out/entry['name'];partial=out/(entry['name']+'.partial')
        if target.exists():
            data=target.read_bytes()
            if len(data)!=entry['bytes'] or f'{zlib.crc32(data):08x}'!=entry['crc32']:raise ValueError('existing file mismatch')
        else:
            offset=partial.stat().st_size if partial.exists() else 0
            if offset>entry['bytes']:raise ValueError('partial larger than source')
            def block(start,length):
                nonlocal transient_failures
                end=start+length-1
                attempts=rule['max_attempts_per_range'] if rule else 1
                for attempt in range(attempts):
                    if request_gap:time.sleep(request_gap)
                    try:
                        status,headers,data=get(ip,f"/file/{entry['id']}",{'Range':f'bytes={start}-{end}'},port=port)
                    except (ConnectionResetError,TimeoutError) as error:
                        if not rule or attempt+1>=attempts or transient_failures>=rule['max_transient_failures_total']:raise
                        status,_,current=get(ip,'/info',port=port)
                        if status!=200 or hashlib.sha256(current).hexdigest()!=fingerprint:raise ValueError('snapshot changed during transport recovery')
                        transient_failures+=1
                        with (out/'transport-events.jsonl').open('a') as log:
                            log.write(json.dumps({'source':'frozen_ram_http_range','policy':recovery_policy,'file':entry['name'],
                                'start':start,'length':length,'exception':type(error).__name__,'fingerprint':fingerprint,
                                'attempt':attempt+2,'failure_count':transient_failures,'measurement_repeated':False})+'\n')
                        continue
                    if status!=206 or headers.get('Content-Range')!=f"bytes {start}-{end}/{entry['bytes']}" or headers.get('ETag')!=f'"{entry["crc32"]}"' or len(data)!=length:raise ValueError('RAM range mismatch')
                    return data
            if offset:
                with partial.open('rb') as file:
                    for start in sorted({0,max(0,offset-4096)}):
                        n=min(4096,offset-start);file.seek(start)
                        if file.read(n)!=block(start,n):raise ValueError('partial differs from frozen RAM')
            with partial.open('ab') as file:
                while offset<entry['bytes']:
                    n=min(chunk,entry['bytes']-offset);file.write(block(offset,n));file.flush();offset+=n
            data=partial.read_bytes()
            if len(data)!=entry['bytes'] or f'{zlib.crc32(data):08x}'!=entry['crc32']:raise ValueError('native/host CRC mismatch')
            partial.rename(target)
        receipts.append({**entry,'sha256':hashlib.sha256(data).hexdigest()})
        (out/'received-ram-sha256.partial.json').write_text(json.dumps(receipts,indent=2)+'\n')
        print('Verified RAM',entry['name'],entry['bytes'],entry['crc32'],flush=True)
    (out/'received-ram-sha256.json').write_text(json.dumps(receipts,indent=2)+'\n')
    print('RAM RECOVERY COMPLETE',prefix['run'],len(files),'files; device complete='+prefix['complete'],'failed='+prefix['failed'],flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('ip');parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();retrieve(args.ip,args.out)
