#!/usr/bin/env python3
"""Validate driver dumps and raw IPC evidence without treating absent fields as values."""
import argparse,csv,hashlib,json,struct
from collections import Counter,defaultdict
from pathlib import Path

def parse_words(value):
    if len(value)!=512 or any(c not in '0123456789abcdefABCDEF' for c in value):
        raise ValueError('raw response must contain 64 complete words')
    return [int(value[i:i+8],16) for i in range(0,len(value),8)]

def inspect(directory):
    directory=Path(directory)
    manifest=(directory/'manifest.txt').read_text()
    drivers=[]
    for line in manifest.splitlines():
        if not line.startswith('DRIVER_FILE '):continue
        fields=dict(part.split('=',1) for part in line.split()[1:])
        path=directory/f"driver-{fields['name']}-{fields['tag']}-{fields['base']}.bin"
        data=path.read_bytes()
        complete=fields['complete']=='1' and len(data)==int(fields['expected'])==int(fields['saved'])
        constants={f'{value:08x}':[i for i in range(0,len(data)-3,4)
                      if data[i:i+4]==struct.pack('<I',value)]
                   for value in (0xd8a053ea,0x002f0080,0x00300080,0x00002280)}
        drivers.append({'file':path.name,'bytes':len(data),'complete':complete,
                        'sha256':hashlib.sha256(data).hexdigest(),'aligned_constant_offsets':constants})
    ipc=defaultdict(Counter); anomalies=[]; getter_words=defaultdict(set)
    with (directory/'ipc.csv').open(newline='') as f:
        for row in csv.DictReader(f):
            words=parse_words(row['response64'])
            command=int(row['command'],16)
            key=f"s{row['sensor']}:{row['service']}:{command:04x}"
            ipc[key][row['service_result']]+=1
            if words[0]>>16!=command:
                anomalies.append({'key':key,'kind':'wrong_response_command','header':f'{words[0]:08x}'})
            if command in (0x1a,0x1c,8):getter_words[key].add(words[2])
    result={'driver_files':drivers,'ipc_results':{k:dict(v) for k,v in ipc.items()},
            'getter_raw_word2':{k:[f'{v:08x}' for v in sorted(vs)] for k,vs in getter_words.items()},
            'ipc_header_anomalies':anomalies,
            'limits':['A live data dump is not an atomic driver snapshot.',
                      'An embedded constant alone does not identify a handler or register map.',
                      'Service acquisition does not prove a command is implemented or permitted.']}
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path)
    args=parser.parse_args();result=inspect(args.directory)
    target=args.directory/'driver-analysis.json';target.write_text(json.dumps(result,indent=2)+'\n')
    print(f"Driver regions: {len(result['driver_files'])}; complete: {sum(d['complete'] for d in result['driver_files'])}; report: {target}")
if __name__=='__main__':main()
