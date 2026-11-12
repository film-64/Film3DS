"""Real control/data sockets exercise ftpd's bounded transfer/ABOR synchronization."""
import ftplib,importlib.util,io,socket,threading
from pathlib import Path
spec=importlib.util.spec_from_file_location('recovery',Path(__file__).parents[1]/'tools/retrieve_probe_ftp.py')
recovery=importlib.util.module_from_spec(spec);spec.loader.exec_module(recovery)
payload=bytes(range(256))*4096

def trial(bad_reply=False):
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(5)
    port=listener.getsockname()[1];commands=[];errors=[]
    def server():
        try:
            with listener,listener.accept()[0] as control:
                control.settimeout(5);stream=control.makefile('rb');control.sendall(b'220 Ready\r\n');start=0
                while line:=stream.readline():
                    command=line.decode().strip();commands.append(command);verb=command.split()[0]
                    if verb=='USER':control.sendall(b'331 Password\r\n')
                    elif verb=='PASS':control.sendall(b'230 OK\r\n')
                    elif verb=='TYPE':control.sendall(b'200 OK\r\n')
                    elif verb=='PASV':
                        data_listener=socket.socket();data_listener.bind(('127.0.0.1',0));data_listener.listen();p=data_listener.getsockname()[1]
                        control.sendall(f'227 Passive (127,0,0,1,{p//256},{p%256})\r\n'.encode())
                    elif verb=='REST':start=int(command.split()[1]);control.sendall(b'350 Restart\r\n')
                    elif verb=='RETR':
                        control.sendall(b'150 Data\r\n')
                        with data_listener,data_listener.accept()[0] as data:
                            try:data.sendall(payload[start:])
                            except (BrokenPipeError,ConnectionResetError):pass
                        start=0
                    elif verb=='ABOR':
                        control.sendall(b'225 Aborted\r\n'+(b'425 Failed to establish connection\r\n' if bad_reply else b'425 Transfer aborted\r\n'))
                    elif verb=='NOOP':control.sendall(b'200 OK\r\n')
                    elif verb=='QUIT':control.sendall(b'221 Bye\r\n');break
                    else:raise AssertionError(command)
        except (BrokenPipeError,ConnectionResetError):
            if not bad_reply:errors.append('unexpected control disconnect')
        except BaseException as error:errors.append(repr(error))
    thread=threading.Thread(target=server);thread.start();output=io.BytesIO();ftp=ftplib.FTP()
    try:
        ftp.connect('127.0.0.1',port,timeout=5);ftp.login();ftp.voidcmd('TYPE I')
        if bad_reply:
            try:recovery.persistent_segment(ftp,'frames.csv',0,16384,output)
            except ftplib.error_temp:pass
            else:raise AssertionError('unrelated 425 was swallowed')
            assert output.getvalue()==b''
        else:
            for offset in (0,16384,32768):recovery.persistent_segment(ftp,'frames.csv',offset,16384,output)
            assert output.getvalue()==payload[:49152]
            ftp.quit()
    finally:ftp.close();thread.join(6)
    assert not thread.is_alive() and not errors,errors
    assert commands.count('USER anonymous')==1
    assert all(c.split()[0] in {'USER','PASS','TYPE','PASV','REST','RETR','ABOR','NOOP','QUIT'} for c in commands)

trial();trial(bad_reply=True)
for name in ('../bad','bad\r\nDELE x','bad\\x'):
    try:recovery.persistent_segment(None,name,0,1,io.BytesIO())
    except ValueError:pass
    else:raise AssertionError('unsafe name accepted')
print('Persistent FTP socket tests passed: one control session, exact REST data, abort synchronization, unrelated 425 and injected names rejected')
