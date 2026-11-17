#!/usr/bin/env python3
"""Bounded Camera outgoing IPC observation using kernel syscall catch only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import select
import socket
import struct
import sys
import time
import xml.etree.ElementTree as ET
from exposure_sweep_contract import CONTRACT_MATRIX

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'build/hardware/native-trace'))
from trace_ipc import Remote

SOURCE='camera-writer-trace/v2'
RULE=CONTRACT_MATRIX[SOURCE]


def fields(stop):
    if not stop.startswith('T'):raise ValueError('unexpected debug stop')
    result=dict(part.split(':',1) for part in stop[3:].split(';') if ':' in part)
    for key in ('thread','0','d','e','f'):
        if key not in result:raise ValueError('incomplete stop register proof')
    return result


def word(value):
    if not re.fullmatch('[0-9a-fA-F]{8}',value):raise ValueError('invalid register word')
    return int.from_bytes(bytes.fromhex(value),'little')


def static_buffers(words,read):
    count=(words[0]>>6)&63;translated=words[0]&63
    if 1+count+translated>len(words):raise ValueError('incomplete IPC command')
    result=[];index=1+count;end=index+translated
    while index<end:
        descriptor=words[index]
        if descriptor&15!=2 or index+1>=end:raise ValueError('unknown I2C translated descriptor')
        size=descriptor>>14;pointer=words[index+1]
        if not 0<size<=RULE['buffer_max_bytes'] or not pointer or pointer+size>2**32:raise ValueError('I2C buffer bounds')
        data=read(pointer,size)
        if len(data)!=size:raise ValueError('truncated I2C buffer')
        result.append(dict(index=(descriptor>>10)&15,pointer=pointer,bytes=size,hex=data.hex()))
        index+=2
    return result


def writes(record):
    """Decode covered write forms; preserve unknown commands as uncertainty."""
    w=record['words'];command=w[0]>>16;buffers=record['buffers']
    if command==0x0a:return [],True  # ReadRegister16
    if command in (8,4):
        count=w[3 if command==8 else 4]
        if len(buffers)!=1 or buffers[0]['bytes']!=count:raise ValueError('invalid multi-device write proof')
        devices=list(bytes.fromhex(buffers[0]['hex']))
        if command==8:return [dict(device=d,addr=w[1]&65535,value=w[2]&65535) for d in devices],True
        return [dict(device=d,addr=w[1]&65535,clear=w[2]&65535,set=w[3]&65535,rmw=True) for d in devices],True
    if command==0x0c:
        if len(buffers)!=1 or buffers[0]['bytes']!=2*w[3]:raise ValueError('invalid contiguous write proof')
        values=struct.unpack('<'+'H'*w[3],bytes.fromhex(buffers[0]['hex']))
        return [dict(device=w[1]&255,addr=(w[2]&65535)+2*i,value=v) for i,v in enumerate(values)],True
    return [],False


def run(ip,expected,output):
    output=Path(output);output.mkdir(exist_ok=False)
    (output/'expected-camera-identity.json').write_text(json.dumps(expected,indent=2)+'\n')
    remote=None;gate=None;attached=False;running=False;catch=False;detached=False
    report=dict(source=SOURCE,complete=False,identity_matched=False,stops=0,entries=0,
                detached=False,catch_disabled=False,gate_released=False,breakpoints=0,memory_writes=0)
    tls_cache={};inflight={};ready_rule=RULE['rendezvous_readiness'];start=time.monotonic()
    with (output/'trace.jsonl').open('x',buffering=1) as log:
        def emit(kind,**data):log.write(json.dumps(dict(kind=kind,host_ns=time.monotonic_ns(),**data))+'\n')
        def command(text):
            emit('rsp_command',command=text)
            value=remote.command(text)
            if text.startswith('vAttach;'):
                emit('rsp_reply',reply=value);return value
            for _ in range(8):
                asynchronous=value.startswith(('T','S')) or (value.startswith('O') and value!='OK' and re.fullmatch('[0-9a-fA-F]*',value[1:]))
                if not asynchronous:emit('rsp_reply',reply=value);return value
                emit('async_rsp_before_reply',packet=value)
                report['async_packets_drained']=report.get('async_packets_drained',0)+1
                value=remote.receive()
            raise ValueError('RSP asynchronous packet bound; no command replay')
        def ok(text):
            if command(text)!='OK':raise ValueError('debug command rejected: '+text)
        def read(address,size):
            value=command(f'm{address:x},{size:x}')
            if not re.fullmatch('[0-9a-fA-F]{'+str(size*2)+'}',value):raise ValueError('incomplete debug read')
            return bytes.fromhex(value)
        def resume():
            nonlocal running
            # No-ack mode, one outstanding continue. Stop packets are drained
            # before any subsequent command or detach. No parallel RSP calls.
            remote.sock.sendall(b'$c#63');running=True
        try:
            while time.monotonic()-start<ready_rule['deadline_seconds']:
                try:gate=socket.create_connection((ip,5001),timeout=2);break
                except (ConnectionRefusedError,TimeoutError,OSError):time.sleep(ready_rule['poll_seconds'])
            if gate is None:raise TimeoutError('writer rendezvous readiness deadline')
            gate.settimeout(4);greeting=gate.makefile('rb',buffering=0)
            if greeting.readline(64)!=b'FILMTRACE1 READY\n':raise ValueError('unregistered writer gate')
            emit('gate_ready')
            remote=Remote(ip,4000);remote.sock.settimeout(4)
            features=command('qSupported:multiprocess+')
            if 'QCatchSyscalls+' not in features or 'QStartNoAckMode+' not in features:raise ValueError('missing kernel syscall catch capability')
            ok('!');ok('QStartNoAckMode');remote.acks=False
            xml=bytearray();offset=0
            while True:
                part=command(f'qXfer:osdata:read:processes:{offset:x},fff')
                if not part or part[0] not in ('m','l'):raise ValueError('missing live process identity')
                xml.extend(part[1:].encode());offset+=len(part)-1
                if part[0]=='l':break
                if offset>32768:raise ValueError('process list bounds')
            items=ET.fromstring(xml).findall('item');processes=[{c.attrib['name']:c.text for c in i.findall('column')} for i in items]
            selected=[r for r in processes if r.get('command')=='camera']
            if len(selected)!=1:raise ValueError('ambiguous live Camera process')
            pid=int(selected[0]['pid']);report['pid']=pid
            stop=command(f'vAttach;{pid:x}');attached=True;fields(stop)
            identity_start=time.monotonic();chunks=[]
            for a in range(0x100000,0x115000,512):
                if time.monotonic()-identity_start>=RULE['identity_deadline_seconds']:raise TimeoutError('trace identity deadline')
                chunks.append(read(a,min(512,0x115000-a)))
            text=b''.join(chunks)
            (output/'camera-text.bin').write_bytes(text)
            report['text_sha256']=hashlib.sha256(text).hexdigest()
            report['identity_matched']=report['text_sha256']==expected['text_sha256'] and all(
                text[int(e['address'],16)-0x100000:int(e['address'],16)-0x100000+16].hex()==e['bytes'] for e in expected['entry_bytes'].values())
            if not report['identity_matched']:raise ValueError('live Camera identity mismatch')
            pointer=struct.unpack('<I',read(0x11d638,4))[0]
            if not 0x11d000<=pointer<=0x11dffc or pointer&3:raise ValueError('unregistered I2C object pointer')
            handle=struct.unpack('<I',read(pointer,4))[0]
            if not handle:raise ValueError('missing I2C session')
            report['i2c_handle']=handle;emit('identity',sha256=report['text_sha256'],i2c_handle=handle,pid=pid)
            ok('QCatchSyscalls:1;32');catch=True;resume()
            gate.sendall(b'GO\n');emit('gate_GO')
            deadline=time.monotonic()+RULE['deadline_seconds'];done=False
            while time.monotonic()<deadline and report['stops']<RULE['max_stops']:
                buffered=remote.offset<len(remote.buffer)
                available=[remote.sock] if buffered else select.select([remote.sock,gate],[],[],min(1,max(0,deadline-time.monotonic())))[0]
                if remote.sock in available:
                    stop=remote.receive();running=False;report['stops']+=1
                    f=fields(stop);thread=f['thread'];entry=f.get('syscall_entry')=='32';returned=f.get('syscall_return')=='32'
                    if not (entry or returned):raise ValueError('unexpected traced event')
                    emit('stop',packet=stop)
                    if entry:
                        if thread in inflight:raise ValueError('unpaired syscall entry')
                        if len(inflight)>=RULE['inflight_limit']:raise ValueError('inflight proof bound')
                        inflight[thread]=dict(i2c=word(f['0'])==handle)
                        if word(f['0'])==handle:
                            if thread not in tls_cache:
                                extra=command('qThreadExtraInfo,'+thread)
                                match=re.search(r'TLS: 0x([0-9a-fA-F]+)',bytes.fromhex(extra).decode())
                                if not match:raise ValueError('missing thread TLS')
                                tls_cache[thread]=int(match.group(1),16)+0x80
                            words=list(struct.unpack('<64I',read(tls_cache[thread],256)))
                            buffers=static_buffers(words,read)
                            record=dict(thread=thread,handle=handle,pc=word(f['f']),lr=word(f['e']),sp=word(f['d']),words=words,buffers=buffers)
                            decoded,covered=writes(record);record.update(writes=decoded,covered=covered)
                            if any(0x32d4<=w['addr']<=0x32da for w in decoded):
                                record['stack_hex']=read(record['sp'],RULE['IFP_stack_bytes']).hex()
                            report['entries']+=1;inflight[thread]['entry_id']=report['entries']
                            emit('i2c_entry',entry_id=report['entries'],**record)
                    else:
                        pending=inflight.pop(thread,None)
                        if pending is None:raise ValueError('unpaired syscall return')
                        if pending['i2c']:
                            response=list(struct.unpack('<64I',read(tls_cache[thread],256)))
                            emit('i2c_return',entry_id=pending['entry_id'],thread=thread,register_r0=word(f['0']),service_result=response[1],words=response)
                    resume()
                elif gate in available:
                    line=greeting.readline(64);emit('gate_DONE',line=line.decode(errors='replace'))
                    if line!=b'FILMTRACE1 DONE ok=1\n':raise ValueError('device window incomplete')
                    done=True;break
            if not done:raise TimeoutError('trace deadline or stop cap; incomplete evidence')
            if inflight:raise ValueError('incomplete syscall response evidence')
            report['complete']=True
        finally:
            try:
                if remote and attached:
                    if running:
                        remote.sock.sendall(b'\x03');emit('cleanup_stop',packet=remote.receive());running=False
                    try:
                        if catch:
                            ok('QCatchSyscalls:0');catch=False;report['catch_disabled']=True
                    finally:
                        ok(f'D;{report["pid"]:x}');attached=False;detached=True;report['detached']=True
            finally:
                if remote:remote.close()
                if gate:
                    if detached:
                        gate.sendall(b'RELEASE\n');report['gate_released']=True;emit('gate_RELEASE')
                    gate.close()
                report['passed']=report['complete'] and report['identity_matched'] and report['detached'] and report['catch_disabled'] and report['gate_released']
                (output/'trace-status.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('ip');parser.add_argument('--expected',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();result=run(args.ip,json.loads(args.expected.read_text()),args.out)
    print(json.dumps(result));raise SystemExit(0 if result['passed'] else 2)
