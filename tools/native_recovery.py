"""Create/verify a local program-content archive; never installs or patches a title."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from camera_runtime_contract import CONTRACT_MATRIX

SOURCE = 'film-native-recovery-archive/v1'
TITLE = '0004001000020400'
CXI_SHA = '6bed3f729451eb70b13e3febc69f42bd56998a7bcd676a2c88d0a4f2193fb71d'
CODE_SHA = 'd3ee1acda48e5529c874598b94833b15bb85434f980cab0374656cc6e89fc43e'
NAMES = {'original.cxi', 'original-code.bin', 'original-exheader.bin', 'RECOVERY.txt'}
RECOVERY = '''This is a private program-content archive, NOT a complete console backup.
It is NOT an installable CIA. It contains no photos, gallery savedata or NAND backup.
No installed title was changed to create it.

Development policy: keep the installed original intact; use a version-pinned,
removable loader overlay only after the actual loader/version has been checked.
For a loader-only overlay, close the Camera title, disable that title's added
overlay (preserving any pre-existing patches), then restart the title or reboot.
If the title cannot start, disable the overlay from the SD card using a computer.
This removes a loader modification; it does not restore photos or savedata.
Removing a patch cannot undo persistent files or camera-service state changes.
Do not reinstall a system title or flash NAND from this CXI archive.
Before device testing, separately back up DCIM, Camera savedata and existing title
overlays, and verify a baseline Camera launch and the overlay-removal procedure.
The device rollback procedure has NOT yet been tested by this archive tool.
'''


def digest(data):
    return hashlib.sha256(data).hexdigest()


def contract(source):
    rule = CONTRACT_MATRIX.get(source)
    if source != SOURCE or not rule or rule['compute'] or rule['lane_admission'] or rule['override'] is not None:
        raise ValueError('unknown or invalid archive source')
    if rule['proof'] != 'local_trace' or rule['deployment'] or rule['installed_title_writes']:
        raise ValueError('archive boundary changed')


def checked_inputs(cxi, code):
    if digest(cxi) != CXI_SHA or digest(code) != CODE_SHA:
        raise ValueError('unrecognized original Camera version/hash')
    if cxi[0x100:0x104] != b'NCCH' or struct.unpack_from('<Q',cxi,0x118)[0] != int(TITLE,16):
        raise ValueError('wrong NCCH/title')
    if cxi[0x18d] & 1 == 0:
        raise ValueError('unexpected executable content')


def verify(directory, source=SOURCE):
    contract(source)
    directory = Path(directory)
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('archive must be a real directory')
    allowed = NAMES | {'manifest.json', 'manifest.sha256'}
    if {p.name for p in directory.iterdir()} != allowed or any(p.is_symlink() or not p.is_file() for p in directory.iterdir()):
        raise ValueError('unexpected/missing archive files or symlink')
    encoded = (directory/'manifest.json').read_bytes()
    if (directory/'manifest.sha256').read_text() != digest(encoded)+'\n':
        raise ValueError('archive manifest hash mismatch')
    m = json.loads(encoded)
    if set(m) != {'source','title_id','scope','device_rollback_tested','local_trace','files'}:
        raise ValueError('unknown manifest fields or misplaced proof')
    if m['source'] != source or m['title_id'] != TITLE or m['scope'] != 'program_content_only' or m['device_rollback_tested'] is not False:
        raise ValueError('invalid archive identity/scope')
    if m['local_trace'] != {'source':SOURCE,'action':'copy_verify_new_local_directory','installed_title_writes':False}:
        raise ValueError('missing, fake or misplaced local archive proof')
    if set(m['files']) != NAMES:
        raise ValueError('invalid archive file set')
    for name, expected in m['files'].items():
        content = (directory/name).read_bytes()
        if expected != {'bytes':len(content),'sha256':digest(content)}:
            raise ValueError('archive content mismatch: '+name)
    cxi, code = [(directory/name).read_bytes() for name in ('original.cxi','original-code.bin')]
    checked_inputs(cxi,code)
    if (directory/'original-exheader.bin').read_bytes() != cxi[0x200:0xa00]:
        raise ValueError('exheader is not the original CXI exheader')
    if (directory/'RECOVERY.txt').read_text() != RECOVERY:
        raise ValueError('incorrect recovery scope instructions')
    return m


def create(cxi_path, code_path, out, source=SOURCE):
    contract(source)
    cxi_path, code_path, out = map(Path,(cxi_path,code_path,out))
    cxi,code = cxi_path.read_bytes(),code_path.read_bytes()
    checked_inputs(cxi,code)
    if out.resolve() in (cxi_path.parent.resolve(),code_path.parent.resolve()):
        raise ValueError('archive cannot replace an input directory')
    # Existing and partially written archives are never replaced or silently resumed.
    out.mkdir(parents=True,exist_ok=False)
    payloads = {'original.cxi':cxi,'original-code.bin':code,'original-exheader.bin':cxi[0x200:0xa00],
                'RECOVERY.txt':RECOVERY.encode()}
    for name,content in payloads.items():
        with (out/name).open('xb') as f:
            f.write(content)
    m = dict(source=source,title_id=TITLE,scope='program_content_only',device_rollback_tested=False,
             local_trace=dict(source=source,action='copy_verify_new_local_directory',installed_title_writes=False),
             files={name:dict(bytes=len(content),sha256=digest(content)) for name,content in payloads.items()})
    encoded = (json.dumps(m,indent=2)+'\n').encode()
    with (out/'manifest.json').open('xb') as f:
        f.write(encoded)
    with (out/'manifest.sha256').open('x') as f:
        f.write(digest(encoded)+'\n')
    if digest(cxi_path.read_bytes()) != CXI_SHA or digest(code_path.read_bytes()) != CODE_SHA:
        raise ValueError('input changed while archiving')
    return verify(out,source)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    commands=p.add_subparsers(dest='command',required=True)
    c=commands.add_parser('create')
    c.add_argument('--cxi',type=Path,required=True);c.add_argument('--code',type=Path,required=True);c.add_argument('--out',type=Path,required=True)
    v=commands.add_parser('verify');v.add_argument('directory',type=Path)
    a=p.parse_args()
    result=create(a.cxi,a.code,a.out) if a.command=='create' else verify(a.directory)
    print(json.dumps(dict(verified=True,title_id=result['title_id'],scope=result['scope'],device_rollback_tested=False)))
