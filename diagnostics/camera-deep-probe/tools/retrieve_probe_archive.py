#!/usr/bin/env python3
"""Pull bounded ranges of an SD-only gzip archive; verify native CRCs and retain raw files."""
import argparse
import hashlib
import http.client
import json
import tarfile
import zlib
from pathlib import Path

def request(ip,port,path,headers=None):
    connection=http.client.HTTPConnection(ip,port,timeout=30)
    try:
        connection.request('GET',path,headers=headers or {})
        response=connection.getresponse()
    except BaseException:
        connection.close();raise
    return connection,response

def extract(archive,out):
    out=Path(out);records=[];seen=set();index=None;total=0
    with tarfile.open(archive,'r:gz') as tar:
        for item in tar:
            name=item.name
            if not item.isfile() or not name or '/' in name or '\\' in name or name.startswith('.') or name in seen:
                raise ValueError('unsafe/duplicate archive member')
            seen.add(name);total+=item.size
            if len(seen)>2049 or item.size>512*1024**2 or total>1024**3:raise ValueError('archive limits exceeded')
            stream=tar.extractfile(item)
            if name=='RECOVERY_INDEX.tsv':
                if item.size>2048*160:raise ValueError('oversized native index')
                index={}
                for line in stream.read().decode('ascii').splitlines():
                    key,size,crc=line.split('\t')
                    if key in index:raise ValueError('duplicate index entry')
                    index[key]=(int(size),crc.lower())
                continue
            if Path(name).suffix not in {'.csv','.txt','.pgm','.yuyv','.dmp','.dump'}:raise ValueError('unexpected file extension')
            temporary=out/(name+'.archive-partial');crc=0;sha=hashlib.sha256();count=0
            with temporary.open('wb') as target:
                while block:=stream.read(65536):
                    target.write(block);sha.update(block);crc=zlib.crc32(block,crc);count+=len(block)
            if count!=item.size:raise ValueError('truncated archive member')
            records.append({'name':name,'bytes':count,'sha256':sha.hexdigest(),'native_crc32':f'{crc:08x}'})
    # Force gzip trailer validation: tar iteration itself may stop at its EOF blocks.
    import gzip
    with gzip.open(archive,'rb') as gz:
        while gz.read(65536):pass
    if index is None or set(index)!={r['name'] for r in records}:raise ValueError('native index coverage mismatch')
    for r in records:
        if index[r['name']]!=(r['bytes'],r['native_crc32']):raise ValueError('native CRC/length mismatch: '+r['name'])
        path=out/r['name'];temporary=out/(r['name']+'.archive-partial')
        if path.exists():
            if path.stat().st_size!=r['bytes'] or hashlib.sha256(path.read_bytes()).hexdigest()!=r['sha256']:
                raise ValueError('existing raw file differs: '+r['name'])
            temporary.unlink()
        else:temporary.replace(path)
    (out/'received-sha256.json').write_text(json.dumps(records,indent=2)+'\n')
    return records

def retrieve(ip,out,port=5000):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    connection,response=request(ip,port,'/info')
    try:
        if response.status!=200:raise ValueError('recovery info rejected')
        text=response.read(1024).decode('ascii').splitlines()
    finally:connection.close()
    if len(text)!=2 or text[0]!='FILMRECOVERY 1':raise ValueError('unknown recovery server')
    size,crc=text[1].split();size=int(size);int(crc,16)
    if not 0<size<=1024**3 or len(crc)!=8:raise ValueError('invalid native archive information')
    identity={'device':ip,'bytes':size,'native_archive_crc32':crc}
    state=out/'archive-source.json';archive=out/'recovery.tar.gz';partial=out/'recovery.tar.gz.partial'
    if state.exists() and json.loads(state.read_text())!=identity:raise ValueError('native archive changed; use a new output directory')
    state.write_text(json.dumps(identity,indent=2)+'\n')
    if not archive.exists():
        offset=partial.stat().st_size if partial.exists() else 0
        if offset>size:raise ValueError('oversized local archive prefix')
        with partial.open('ab') as target:
            while offset<size:
                end=min(offset+262144,size)-1
                connection,response=request(ip,port,'/archive',{'Range':f'bytes={offset}-{end}'})
                try:
                    if (response.status!=206 or response.getheader('Content-Range')!=f'bytes {offset}-{end}/{size}'
                            or response.getheader('ETag')!=f'"{crc}"' or int(response.getheader('Content-Length','-1'))!=end-offset+1):
                        raise ValueError('range response does not match native archive')
                    remaining=end-offset+1
                    while remaining:
                        try:block=response.read(min(65536,remaining))
                        except http.client.IncompleteRead as error:
                            target.write(error.partial);raise
                        if not block:raise EOFError('truncated archive range')
                        target.write(block);remaining-=len(block);offset+=len(block)
                    target.flush();print('Compressed archive',offset,'/',size,flush=True)
                finally:connection.close()
        partial.replace(archive)
    checksum=0
    with archive.open('rb') as file:
        while block:=file.read(65536):checksum=zlib.crc32(block,checksum)
    if archive.stat().st_size!=size or f'{checksum:08x}'!=crc:raise ValueError('native archive CRC/length mismatch')
    records=extract(archive,out)
    (out/'archive-verification.json').write_text(json.dumps({'complete':True,'native_archive_crc32':crc,
        'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'files':len(records),
        'native_file_crc32_verified':True,'originals':'retained on SD; no camera access'},indent=2)+'\n')
    print('ARCHIVE RECOVERY COMPLETE',len(records),'files; native CRCs verified',flush=True)
    return records

def retrieve_ftp(ip,out,run):
    """Retrieve an already-created archive through ftpd, without camera work."""
    import re
    from retrieve_probe_ftp import connect,segment
    if not re.fullmatch(r'run-[0-9]+-[0-9]+',run):raise ValueError('invalid run directory')
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    source='/3ds/Film3DS/recovery';name=run+'.tar.gz'
    ftp=connect(ip,source)
    try:size=ftp.size(name)
    finally:ftp.close()
    if size is None or not 0<size<1024**3:raise ValueError('invalid archive size')
    identity={'device':ip,'source':source+'/'+name,'bytes':size,'method':'read-only FTP REST; native per-file CRC verification'}
    state=out/'archive-ftp-source.json'
    if state.exists() and json.loads(state.read_text())!=identity:raise ValueError('archive source changed')
    state.write_text(json.dumps(identity,indent=2)+'\n')
    archive=out/'recovery-ftp.tar.gz';partial=out/'recovery-ftp.tar.gz.partial'
    if not archive.exists():
        offset=partial.stat().st_size if partial.exists() else 0
        if offset>size:raise ValueError('oversized archive prefix')
        with partial.open('ab') as target:
            while offset<size:
                length=min(262144,size-offset);segment(ip,source,name,offset,length,target)
                offset+=length;target.flush();print('Archive downloaded',offset,'/',size,flush=True)
        partial.replace(archive)
    if archive.stat().st_size!=size:raise ValueError('archive size mismatch')
    records=extract(archive,out)
    (out/'archive-verification.json').write_text(json.dumps({'complete':True,'files':len(records),
        'native_file_crc32_verified':True,'gzip_trailer_verified':True,
        'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'source':identity},indent=2)+'\n')
    print('ARCHIVE RECOVERY COMPLETE',len(records),'files; native file CRCs verified',flush=True)
    return records

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('ip');parser.add_argument('--out',type=Path,required=True);parser.add_argument('--port',type=int,default=5000)
    parser.add_argument('--ftp-run',help='read an existing SD recovery archive via ftpd instead of the recovery HTTP service')
    args=parser.parse_args()
    if args.ftp_run:retrieve_ftp(args.ip,args.out,args.ftp_run)
    else:retrieve(args.ip,args.out,args.port)
if __name__=='__main__':main()
