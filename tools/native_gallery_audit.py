"""Version-pinned static gallery/decoder evidence. Does not approve or emit hooks."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from camera_runtime_contract import CONTRACT_MATRIX
from native_recovery import CODE_SHA

SOURCE='film-native-gallery-audit/v1'
BASE=0x100000
TEXT_BYTES=0x246a34
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT.parent/'.tools/film3ds-python'))
from capstone import Cs,CS_ARCH_ARM,CS_MODE_ARM

# Labels describe observations, not recovered original symbol names.
CLASSES={
    'JpegDecoder':(0x41b799,b'N5imgdb11JpegDecoderE',0x41b184,0x41dc34),
    'MpDecoder':(0x41b7cb,b'N5imgdb9MpDecoderE',0x41b1ac,0x41de40),
    'SceneBrowse':(0x41be91,b'N5notes5pnote11SceneBrowseE',0x41b4bc,0x41f8d8),
    'ScenePaint':(0x41be3f,b'N5notes5pnote10ScenePaintE',0x41b498,0x41f6fc),
}
NODES={
    'jpeg_constructor_owned_output':0x255520,'jpeg_constructor_borrowed_output':0x1ffd54,
    'mp_constructor_owned_output':0x22763c,'mp_constructor_borrowed_output':0x1ffe2c,
    'jpeg_prepare_header':0x25543c,'jpeg_release':0x2550c4,'mp_release':0x2275a8,
    'jpeg_cancel_request':0x240210,'mp_cancel_request':0x1ffd4c,
    'shared_cancel_primitive':0x240218,'load_request_create':0x1ff5a8,
    'load_wait_completed':0x1fffa0,'load_release_by_id':0x1f5e50,
    'load_get_decoder':0x20b0b4,'load_query_state':0x20b0d8,
    'load_get_extra_dimensions':0x2bb330,'load_worker':0x2baf78,
    'consumer_poll':0x2ef0f0,'consumer_collect_then_release':0x2ef150,
    'consumer_key_setter':0x2eee68,'opaque_u64_key_less_than':0x2096f4,
    'encoded_buffer_lookup_by_key':0x20b13c,'key_kind_accessor':0x22a158,
    'scene_stack_release_pass':0x300158,
}
EDGES={
    0x2bb140:0x1ffe2c,0x2bb254:0x1ffd54,0x2ef09c:0x20b13c,
    0x2bb194:0x2013e8,0x2bb2a8:0x22bab8,
    0x2ef0cc:0x1ff5a8,0x2ef100:0x20b0d8,0x2ef128:0x1f5e50,
    0x2ef198:0x2bb330,0x2ef1bc:0x20b0b4,0x2ef220:0x1f5e50,
    0x1f5f24:0x1ffd4c,0x1f5f8c:0x240210,
}
REGIONS={
    'borrowed_jpeg_constructor':(0x1ffd54,0x1ffe28),
    'borrowed_mpo_constructor':(0x1ffe2c,0x1fff0c),
    'worker_member_dispatch':(0x2bb110,0x2bb2fc),
    'consumer_state_poll':(0x2ef0f0,0x2ef14c),
    'consumer_collect_release':(0x2ef150,0x2ef248),
    'jpeg_cancel':(0x240210,0x24023c),
    'scene_stack_release':(0x300158,0x300238),
    'opaque_key_copy':(0x2eee68,0x2eeeb8),
    'opaque_key_compare':(0x2096f4,0x209718),
    'encoded_buffer_lookup_result':(0x20b200,0x20b21c),
}


def rule(source):
    r=CONTRACT_MATRIX.get(source)
    if source!=SOURCE or not r or r['compute'] or r['lane_admission'] or r['override'] is not None or r['proof']!='local_trace':
        raise ValueError('unregistered or incorrect static audit source')


def analyze(data,source=SOURCE):
    rule(source)
    if hashlib.sha256(data).hexdigest()!=CODE_SHA:raise ValueError('unrecognized native code version')
    md=Cs(CS_ARCH_ARM,CS_MODE_ARM)
    def word(address):return struct.unpack_from('<I',data,address-BASE)[0]
    def instructions(start,end):
        return [dict(address=f'{i.address:08X}',mnemonic=i.mnemonic,operands=i.op_str)
                for i in md.disasm(data[start-BASE:end-BASE],start)]
    classes={}
    for label,(address,name,ti,vt) in CLASSES.items():
        if data[address-BASE:address-BASE+len(name)+1]!=name+b'\0' or word(ti+4)!=address or word(vt-4)!=ti or word(vt-8)!=0:
            raise ValueError('RTTI/vtable profile mismatch: '+label)
        slots=(0,1) if label.endswith('Decoder') else (11,12,14,15,32,33,34)
        classes[label]=dict(name_address=f'{address:08X}',typeinfo=f'{ti:08X}',vtable=f'{vt:08X}',
                            selected_slots={str(i):f'{word(vt+4*i):08X}' for i in slots})
    branches={}
    callers={name:[] for name in NODES}
    for off in range(0,TEXT_BYTES,4):
        w=word(BASE+off)
        if (w&0x0f000000)!=0x0b000000 or w>>28==15:continue
        delta=w&0xffffff
        if delta&0x800000:delta-=0x1000000
        target=BASE+off+8+4*delta
        branches[BASE+off]=target
        for name,address in NODES.items():
            if target==address:callers[name].append(f'{BASE+off:08X}')
    for site,target in EDGES.items():
        if branches.get(site)!=target:raise ValueError('call edge mismatch')
    checks={
        0x255548:('stm','r6, {r0, r1, r2}'),
        0x1ffd80:('stm','r8, {r0, r1, r2}'),
        0x1ffe60:('stm','lr, {r0, r1, r2}'),
        0x1ffdd0:('strb','r0, [r4, #0x52]'),
        0x1ffeac:('strb','r0, [r4, #0x52]'),
        0x240234:('strbeq','r1, [r0, #0x45]'),
        0x300200:('mov','r0, r5'),
        0x300204:('blx','r1'),
        0x2eeeac:('ldrd','r0, r1, [r5]'),
        0x2eeeb0:('strd','r0, r1, [r4, #0xc8]'),
        0x2ef0d4:('str','r0, [r5, #0xc4]'),
    }
    for address,(mnemonic,operands) in checks.items():
        found=instructions(address,address+4)
        if len(found)!=1 or (found[0]['mnemonic'],found[0]['operands'])!=(mnemonic,operands):
            raise ValueError('instruction anchor mismatch')
    state_pairs={0x34b1d8:0x2eefb4,0x34b200:0x2ef0f0,0x34b208:0x2ef150}
    for address,callback in state_pairs.items():
        if (word(address),word(address+4))!=(callback,0):raise ValueError('state callback pair mismatch')
    return dict(source=source,profile='CTR-N-HEPJ_JP_remaster0005',code_sha256=CODE_SHA,
        local_trace=dict(source=source,input_sha256=CODE_SHA,execution='static_only'),
        classes=classes,nodes={name:dict(address=f'{a:08X}',direct_call_sites=callers[name]) for name,a in NODES.items()},
        checked_edges=[dict(site=f'{s:08X}',target=f'{t:08X}') for s,t in EDGES.items()],
        state_callback_pairs={f'{a:08X}':dict(callback=f'{f:08X}',second_word=0) for a,f in state_pairs.items()},
        instruction_anchors={f'{a:08X}':instructions(a,a+4)[0] for a in checks},
        regions={name:instructions(*span) for name,span in REGIONS.items()},
        observations=[
            'Constructors store r1/r2 at object+0x1c/0x20; JPEG header preparation reads these fields.',
            'Borrowed-output constructors set object+0x52=0; owning constructors set it to 1.',
            'Worker dispatches JPEG and MPO requests to distinct constructors and decode routines.',
            'Consumer observes request state, collects results, then releases by request ID.',
            'Cancellation sets a request byte; it is not proof of completion or safe buffer release.',
            'SceneManager has a separate virtual scene cleanup/destruction pass.',
            'An opaque 8-byte media key is copied to consumer+0xc8; native comparison uses unsigned 64-bit ordering.',
            'The temporary load request ID at consumer+0xc4 is distinct from that opaque media key.',
            'Lookup by the opaque media key supplies a buffer/length pair used to construct a decode request.'
        ],limits=dict(hook_approved=False,native_execution=False,complete_callgraph=False,
            selected_photo_identity_proven=False,buffer_layout_proven=False,return_path_proven=False,
            reason='UI selection origin, raster layout, ownership and native state transitions still require correlation'))


def create(code,out,source=SOURCE):
    code,out=Path(code),Path(out)
    data=code.read_bytes();result=analyze(data,source)
    out.parent.mkdir(parents=True,exist_ok=True)
    with out.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
    if hashlib.sha256(code.read_bytes()).hexdigest()!=CODE_SHA:raise ValueError('input changed during audit')
    return result


def verify(code,report):
    data=Path(code).read_bytes();found=json.loads(Path(report).read_text())
    if found!=analyze(data):raise ValueError('static evidence differs from pinned code or contains unsupported claims')
    return found


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--code',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--verify',action='store_true')
    a=p.parse_args();r=verify(a.code,a.out) if a.verify else create(a.code,a.out)
    print(json.dumps(dict(verified=True,profile=r['profile'],classes=len(r['classes']),
                         checked_edges=len(r['checked_edges']),hook_approved=False)))
