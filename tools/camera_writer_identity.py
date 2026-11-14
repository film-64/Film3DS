#!/usr/bin/env python3
"""One bounded Camera text read; no writes, breakpoints, capture or retries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import time
from exposure_sweep_contract import CONTRACT_MATRIX

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'build/hardware/native-trace'))
from trace_gdb_mi import Debugger

SOURCE='camera-writer-identity/v1'


def console_text(lines):
    return ''.join(json.loads(line[1:]) for line in lines if line.startswith('~'))


def identify(debugger,expected,output,ip,clock=time.monotonic,source=SOURCE):
    if source!=SOURCE or source not in CONTRACT_MATRIX:raise ValueError('unregistered identity source')
    rule=CONTRACT_MATRIX[source];start,end=rule['read_range']
    if expected['base']!=f'{start:08x}' or expected['text_bytes']!=end-start:raise ValueError('unregistered read range')
    for e in expected['entry_bytes'].values():
        address=int(e['address'],16)
        if not start<=address<=end-16 or len(bytes.fromhex(e['bytes']))!=16:raise ValueError('invalid entry signature')
    output=Path(output);output.mkdir(exist_ok=False)
    report=dict(source=source,attachment_attempted=False,attached=False,detached=False,
                identity_matched=False,captured_bytes=0,passed=False,writes=0,breakpoints=0)
    data=bytearray();paused=None
    with (output/'gdb-mi.jsonl').open('x') as log:
        def command(text):
            log.write(json.dumps(dict(command=text,at=clock()))+'\n');log.flush()
            answer=debugger.command(text,rule['command_timeout_seconds'])
            log.write(json.dumps(dict(answer=answer,at=clock()))+'\n');log.flush()
            return answer
        try:
            for setting in ('pagination off','confirm off','tcp connect-timeout 4','remotetimeout 4','sysroot /nonexistent'):
                command('-interpreter-exec console '+json.dumps('set '+setting))
            command(f'-target-select extended-remote {ip}:4000')
            processes=console_text(command('-interpreter-exec console "info os processes"'))
            (output/'processes.txt').write_text(processes)
            pids=re.findall(r'^\s*(\d+)\s+camera\s*$',processes,re.M)
            if len(pids)!=1:raise ValueError('Camera process identity missing or ambiguous')
            report['debugger_pid']=int(pids[0]);report['attachment_attempted']=True;paused=clock()
            answer=command('-target-attach '+pids[0]);report['attached']=True
            debugger.stop(answer,rule['command_timeout_seconds'])
            for address in range(start,end,rule['read_chunk_bytes']):
                if clock()-paused>=rule['pause_deadline_seconds']:raise TimeoutError('identity pause deadline')
                size=min(rule['read_chunk_bytes'],end-address)
                answer=command(f'-data-read-memory-bytes 0x{address:x} {size}')
                encoded=re.findall(r'contents="([0-9a-fA-F]+)"',' '.join(answer))
                if len(encoded)!=1 or len(encoded[0])!=size*2:raise ValueError('incomplete live Camera text')
                data.extend(bytes.fromhex(encoded[0]));report['captured_bytes']=len(data)
            actual=hashlib.sha256(data).hexdigest();report['text_sha256']=actual
            report['entries']={name:data[int(e['address'],16)-start:int(e['address'],16)-start+16].hex()==e['bytes'] for name,e in expected['entry_bytes'].items()}
            report['identity_matched']=actual==expected['text_sha256'] and all(report['entries'].values())
        finally:
            try:
                if report['attachment_attempted']:
                    answer=command('-target-detach')
                    report['detached']=any('^done' in line for line in answer)
            finally:
                if paused is not None:report['pause_seconds']=clock()-paused
                (output/'camera-text.bin').write_bytes(data)
                report['passed']=report['identity_matched'] and report['detached']
                (output/'identity.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('ip');parser.add_argument('--gdb',required=True)
    parser.add_argument('--expected',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();debugger=Debugger(args.gdb)
    try:
        result=identify(debugger,json.loads(args.expected.read_text()),args.out,args.ip)
        print(json.dumps(result));raise SystemExit(0 if result['passed'] else 2)
    finally:
        try:debugger.command('-gdb-exit',3)
        finally:debugger.close()
