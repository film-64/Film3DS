"""Exercise REST recovery against an actual local FTP control/data socket pair."""
import importlib.util
import io
import socket
import threading
from pathlib import Path

spec=importlib.util.spec_from_file_location('recover',Path(__file__).parents[1]/'tools/retrieve_probe_ftp.py')
recover=importlib.util.module_from_spec(spec);spec.loader.exec_module(recover)
payload=bytes(range(256))*4096

def trial(offset,length,truncate=False):
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(5)
    port=listener.getsockname()[1];commands=[];errors=[]
    def serve():
        try:
            with listener,listener.accept()[0] as control:
                control.settimeout(5);stream=control.makefile('rb');control.sendall(b'220 Test\r\n')
                data_listener=None;start=0
                while line:=stream.readline():
                    command=line.decode().strip();commands.append(command);verb=command.split()[0]
                    if verb=='USER':control.sendall(b'331 Password\r\n')
                    elif verb in ('PASS','TYPE','CWD'):control.sendall(b'230 OK\r\n' if verb=='PASS' else b'200 OK\r\n')
                    elif verb=='PASV':
                        data_listener=socket.socket();data_listener.bind(('127.0.0.1',0));data_listener.listen();p=data_listener.getsockname()[1]
                        control.sendall(f'227 Passive (127,0,0,1,{p//256},{p%256})\r\n'.encode())
                    elif verb=='REST':start=int(command.split()[1]);control.sendall(b'350 Restart\r\n')
                    elif verb=='RETR':
                        control.sendall(b'150 Data\r\n')
                        with data_listener,data_listener.accept()[0] as data:
                            try:data.sendall(payload[start:start+17] if truncate else payload[start:])
                            except (BrokenPipeError,ConnectionResetError):pass
                        break
                    else:raise AssertionError(command)
        except BaseException as error:errors.append(error)
    thread=threading.Thread(target=serve);thread.start();target=io.BytesIO()
    try:
        if truncate:
            try:recover.segment('127.0.0.1','/closed-run','reads.csv',offset,length,target,port)
            except EOFError:pass
            else:raise AssertionError('truncated data accepted')
            assert target.getvalue()==payload[offset:offset+17]
        else:
            recover.segment('127.0.0.1','/closed-run','reads.csv',offset,length,target,port)
            assert target.getvalue()==payload[offset:offset+length]
    finally:thread.join(6)
    assert not thread.is_alive() and not errors,errors
    assert ('REST '+str(offset) in commands)==bool(offset),commands
    assert 'RETR reads.csv' in commands
    assert all(c.split()[0] in {'USER','PASS','TYPE','CWD','PASV','REST','RETR'} for c in commands)

trial(0,262144)
trial(692224,262144)
trial(12345,262144,truncate=True)
print('FTP socket tests passed: bounded reads, REST offsets, truncated prefix retention, read-only commands')
