"""Real C memory streams and real TCP HTTP ranges; no mock transport."""
import hashlib,http.client,importlib.util,json,subprocess,sys,tempfile,zlib,socket,time
from pathlib import Path
root=Path(__file__).parents[1];sys.path.insert(0,str(root/'tools'))
import retrieve_probe_ram as recovery
binary=root/'build/ram-log-test';binary.parent.mkdir(exist_ok=True)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',str(root/'tests/ram_log.c'),'-o',str(binary)],check=True)
server=subprocess.Popen([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
    line=server.stdout.readline();assert line.startswith('PORT '),line
    port=int(line.split()[1])
    # A slow reader with a small receive window must not lose queued bytes to
    # an abortive close after the first 1s poll timeout. Exercise the real C FIN
    # drain, not a mocked get().
    with socket.socket() as slow:
        slow.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024);slow.settimeout(15)
        slow.connect(('127.0.0.1',port))
        slow.sendall(b'GET /file/1 HTTP/1.1\r\nHost: test\r\nRange: bytes=0-69999\r\nConnection: close\r\n\r\n')
        time.sleep(2)
        stream=slow.makefile('rb');assert stream.readline().startswith(b'HTTP/1.1 206')
        while stream.readline()!=b'\r\n':pass
        assert stream.read(70000)==bytes(i%251 for i in range(70000))
        stream.close()
    with tempfile.TemporaryDirectory() as temporary:
        out=Path(temporary);recovery.retrieve('127.0.0.1',out,chunk=32768,port=port)
        assert (out/'s1-p003-test.pgm').read_bytes()==bytes(i%251 for i in range(70000))
        assert (out/'manifest.txt').read_bytes()==b'RAM fixture\n' and (out/'lifecycle.txt').stat().st_size==0
        receipts=json.loads((out/'received-ram-sha256.json').read_text())
        assert len(receipts)==3
        for r in receipts:
            b=(out/r['name']).read_bytes();assert hashlib.sha256(b).hexdigest()==r['sha256'] and f'{zlib.crc32(b):08x}'==r['crc32']
        # Prefix validation and resumption against the same frozen snapshot.
        (out/'s1-p003-test.pgm').unlink();(out/'s1-p003-test.pgm.partial').write_bytes(bytes(i%251 for i in range(35000)))
        recovery.retrieve('127.0.0.1',out,chunk=32768,port=port)
        assert (out/'s1-p003-test.pgm').stat().st_size==70000
    for path,headers in [('/file/999',{'Range':'bytes=0-1'}),('/file/1',{'Range':'bytes=0-70000'}),('/file/../1',{'Range':'bytes=0-1'})]:
        status,_,_=recovery.get('127.0.0.1',path,headers,port=port);assert status==400
    try:recovery.parse_index(b'FILMRAM 1\nrun x\ncomplete 1\nfailed 0\ncount 1\n0 ../evil.csv 1 00000000\n')
    except ValueError:pass
    else:raise AssertionError('unsafe filename accepted')
finally:
    server.terminate();server.communicate(timeout=5)
print('PASS RAM evidence: bounded memory, overflow and frozen-write rejection, native/host CRC, real HTTP ranges, zero files, resume and invalid requests')
