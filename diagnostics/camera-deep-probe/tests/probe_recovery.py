"""Run the real recovery C archive/server against the real Python range client."""
import hashlib
import http.client
import importlib.util
import io
import json
import os
import socket
import subprocess
import tarfile
import tempfile
import time
import zlib
from pathlib import Path

root=Path(__file__).parents[1]
spec=importlib.util.spec_from_file_location('recovery',root/'tools/retrieve_probe_archive.py')
recovery=importlib.util.module_from_spec(spec);spec.loader.exec_module(recovery)

with tempfile.TemporaryDirectory() as tmp:
    directory=Path(tmp);binary=directory/'recovery';run='run-123456-000'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(root/'probes/recovery/main.c'),'-lz','-o',str(binary)],check=True)
    source=directory/'sd'/'probes'/run;source.mkdir(parents=True)
    originals={'manifest.txt':b'END complete=1\n','ipc.csv':b'raw IPC\n'*10000,'s3-p034-test.pgm':os.urandom(1100000)}
    for name,data in originals.items():(source/name).write_bytes(data)
    bad=subprocess.run([str(binary),'--run=../bad','--root='+str(directory/'sd'),'--pack-only'],capture_output=True)
    assert bad.returncode==2
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    with (directory/'server.log').open('wb') as log:
        server=subprocess.Popen([str(binary),'--run='+run,'--root='+str(directory/'sd'),'--port='+str(port)],stdout=log,stderr=log)
        try:
            for _ in range(100):
                try:
                    c,r=recovery.request('127.0.0.1',port,'/info');info=r.read().decode().splitlines();c.close();break
                except OSError:time.sleep(.05)
            else:raise AssertionError('server failed to start')
            size,crc=info[1].split();native=directory/'sd'/'recovery'/(run+'.tar.gz')
            out=directory/'out';out.mkdir()
            identity={'device':'127.0.0.1','bytes':int(size),'native_archive_crc32':crc}
            (out/'archive-source.json').write_text(json.dumps(identity))
            (out/'recovery.tar.gz.partial').write_bytes(native.read_bytes()[:12345])
            (out/'manifest.txt').write_bytes(originals['manifest.txt'])
            records=recovery.retrieve('127.0.0.1',out,port)
            assert len(records)==len(originals)
            for name,data in originals.items():
                assert (out/name).read_bytes()==data and (source/name).read_bytes()==data
            c,r=recovery.request('127.0.0.1',port,'/archive',{'Range':'bytes=0-999999'});assert r.status==400;r.read();c.close()
            c,r=recovery.request('127.0.0.1',port,'/../../outside',{'Range':'bytes=0-10'});assert r.status==400;r.read();c.close()
        finally:server.terminate();server.wait(timeout=5)
    before=hashlib.sha256(native.read_bytes()).hexdigest()
    duplicate=subprocess.run([str(binary),'--run='+run,'--root='+str(directory/'sd'),'--pack-only'],capture_output=True)
    assert duplicate.returncode==1 and b'already exists' in duplicate.stdout
    reuse=subprocess.run([str(binary),'--run='+run,'--root='+str(directory/'sd'),'--serve-existing','--pack-only'],capture_output=True)
    assert reuse.returncode==0 and hashlib.sha256(native.read_bytes()).hexdigest()==before
    # An incorrect native index must never replace an existing raw file.
    bad_archive=directory/'bad.tar.gz';target=directory/'invalid';target.mkdir();(target/'manifest.txt').write_bytes(b'keep')
    with tarfile.open(bad_archive,'w:gz') as tar:
        for name,data in [('manifest.txt',b'changed'),('RECOVERY_INDEX.tsv',b'manifest.txt\t7\t00000000\n')]:
            item=tarfile.TarInfo(name);item.size=len(data);tar.addfile(item,io.BytesIO(data))
    try:recovery.extract(bad_archive,target)
    except ValueError:pass
    else:raise AssertionError('bad native CRC accepted')
    assert (target/'manifest.txt').read_bytes()==b'keep'
print('Recovery integration passed: actual C gzip/server, HTTP resume, native file CRCs, range/path rejection, raw originals preserved')
