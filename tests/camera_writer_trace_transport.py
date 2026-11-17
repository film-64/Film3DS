"""Real TCP/RSP lifecycle test. Device and kernel are explicit test peers."""
import hashlib
import json
from pathlib import Path
import select
import socket
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import camera_writer_trace as trace


class Peer:
    def __init__(self):
        self.text=bytes(range(256))*336;self.current=None;self.stop_index=0
        self.all_returned=threading.Event();self.commands=[];self.gate_commands=[];self.failures=[]
        self.async_cleanup=False;self.raw_return_argument=False
        self.gdb=socket.socket();self.gdb.bind(('127.0.0.1',0));self.gdb.listen(1)
        self.gate=socket.socket();self.gate.bind(('127.0.0.1',0));self.gate.listen(1)
        self.threads=[threading.Thread(target=self.wrapper,args=(self.rsp,),daemon=True),threading.Thread(target=self.wrapper,args=(self.window,),daemon=True)]
        for t in self.threads:t.start()
    def wrapper(self,target):
        try:target()
        except BaseException as error:self.failures.append(error)
    def stop(self,entry=True):
        values={'0':0x50009 if entry else (0x32d4+2*self.current[0] if self.raw_return_argument else 0),'d':0x0fffe000,'e':0x10fa3c,'f':0x10bf90}
        return 'T05thread:p17.1;'+''.join(k+':'+struct.pack('<I',v).hex()+';' for k,v in values.items())+('syscall_entry' if entry else 'syscall_return')+':32;'
    def packet(self,conn,payload):
        payload=payload.encode();conn.sendall(b'$'+payload+b'#'+f'{sum(payload)%256:02x}'.encode())
    def rsp(self):
        with self.gdb.accept()[0] as conn:
            conn.settimeout(10);ack=True
            while True:
                marker=conn.recv(1)
                if not marker:return
                if marker==b'+':continue
                if marker==b'\x03':
                    self.packet(conn,self.stop() if self.async_cleanup else self.stop().replace('T05','T02').split('syscall_')[0])
                    if self.async_cleanup:self.packet(conn,'S02')
                    continue
                if marker!=b'$':raise ValueError('bad RSP frame')
                payload=bytearray()
                while True:
                    c=conn.recv(1)
                    if c==b'#':break
                    payload.extend(c)
                checksum=conn.recv(2);assert int(checksum,16)==sum(payload)%256
                command=payload.decode();self.commands.append(command)
                if ack:conn.sendall(b'+')
                if command.startswith('qSupported'):reply='PacketSize=4000;QStartNoAckMode+;QCatchSyscalls+;multiprocess+'
                elif command=='!':reply='OK'
                elif command=='QStartNoAckMode':reply='OK';ack=False
                elif command.startswith('qXfer'):reply='l<osdata type="processes"><item><column name="pid">23</column><column name="command">camera</column></item></osdata>'
                elif command.startswith('vAttach'):reply=self.stop().split('syscall_')[0]
                elif command.startswith('qThreadExtraInfo'):reply=b'Thread TLS: 0x1ff82000'.hex()
                elif command.startswith('QCatchSyscalls') or command.startswith('D;'):reply='OK'
                elif command=='c':
                    if self.stop_index<8:
                        self.current=(self.stop_index//2,self.stop_index%2==0);self.stop_index+=1;reply=self.stop(self.current[1])
                    else:self.all_returned.set();continue
                elif command.startswith('m'):
                    address,size=(int(v,16) for v in command[1:].split(','));data=None
                    if 0x100000<=address<0x115000:data=self.text[address-0x100000:address-0x100000+size]
                    elif address==0x11d638:data=struct.pack('<I',0x11d2b4)
                    elif address==0x11d2b4:data=struct.pack('<I',0x50009)
                    elif address==0x8000000:data=b'\2'
                    elif address==0x0fffe000:data=bytes(size)
                    elif address==0x1ff82080:
                        index,entry=self.current
                        words=([(8<<16)|(3<<6)|2,0x32d4+index*2,128,1,(1<<14)|2,0x8000000]+[0]*58) if entry else [0x80040,0]+[0]*62
                        data=struct.pack('<64I',*words)
                    if data is None or len(data)!=size:raise ValueError('unregistered read '+command)
                    reply=data.hex()
                else:raise ValueError('unregistered RSP command '+command)
                self.packet(conn,reply)
    def window(self):
        with self.gate.accept()[0] as conn:
            conn.settimeout(10);conn.sendall(b'FILMTRACE1 READY\n');line=conn.recv(64);self.gate_commands.append(line)
            if line!=b'GO\n':return
            while not self.all_returned.wait(.01):
                if select.select([conn],[],[],0)[0]:self.gate_commands.append(conn.recv(64));return
            conn.sendall(b'FILMTRACE1 DONE ok=1\n');self.gate_commands.append(conn.recv(64))
    def connect(self,old,address,timeout):
        ip,port=address
        return old(('127.0.0.1',(self.gdb if port==4000 else self.gate).getsockname()[1]),timeout=timeout)
    def close(self):
        for t in self.threads:t.join(3)
        self.gdb.close();self.gate.close()


class TransportTests(unittest.TestCase):
    def exercise(self,mismatch=False,async_cleanup=False,raw_argument=False):
        peer=Peer();original=socket.create_connection
        peer.async_cleanup=async_cleanup;peer.raw_return_argument=raw_argument
        expected=dict(text_sha256=hashlib.sha256(peer.text).hexdigest(),entry_bytes=dict(write=dict(address='0010899c',bytes=peer.text[0x899c:0x899c+16].hex())))
        if mismatch:expected['text_sha256']='0'*64
        try:
            with tempfile.TemporaryDirectory() as temp,patch.object(socket,'create_connection',lambda address,timeout=4:peer.connect(original,address,timeout)):
                out=Path(temp)/'trace'
                if mismatch:
                    with self.assertRaisesRegex(ValueError,'identity mismatch'):trace.run('test',expected,out)
                else:
                    result=trace.run('test',expected,out);self.assertTrue(result['passed'],result);self.assertEqual(result['entries'],4)
                    entries=[json.loads(l) for l in (out/'trace.jsonl').read_text().splitlines() if '"kind": "i2c_entry"' in l]
                    self.assertEqual([r['writes'][0]['addr'] for r in entries],[0x32d4,0x32d6,0x32d8,0x32da])
                status=json.loads((out/'trace-status.json').read_text());self.assertTrue(status['detached'])
            peer.close();self.assertFalse(peer.failures,peer.failures)
            self.assertIn('D;17',peer.commands)
            self.assertFalse(any(c.startswith(('M','X','Z')) for c in peer.commands))
            if mismatch:self.assertNotIn(b'GO\n',peer.gate_commands)
            else:self.assertEqual(peer.gate_commands,[b'GO\n',b'RELEASE\n']);self.assertIn('QCatchSyscalls:0',peer.commands)
        finally:
            peer.gdb.close();peer.gate.close()
    def test_real_RSP_entry_return_buffers_cleanup_and_gate(self):self.exercise()
    def test_identity_mismatch_detaches_without_GO(self):self.exercise(mismatch=True)
    def test_queued_stop_drained_without_replaying_disable_command(self):self.exercise(async_cleanup=True)
    def test_svc_return_R0_is_raw_and_not_treated_as_Result(self):self.exercise(raw_argument=True)


if __name__=='__main__':unittest.main()
