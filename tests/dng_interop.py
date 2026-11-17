"""Check the standard TIFF/DNG payload and, when installed, external readers."""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
p=ROOT/'build/standard-sensor.DNG'
data=p.read_bytes()
assert data[:8]==b'II*\0\x08\0\0\0'
n=struct.unpack_from('<H',data,8)[0]
tags={}
for i in range(n):
    pos=10+12*i
    tag,kind,count,value=struct.unpack_from('<HHII',data,pos)
    size={1:1,2:1,3:2,4:4,5:8,10:8}[kind]*count
    tags[tag]=data[pos+8:pos+8+size] if size<=4 else data[value:value+size]
assert tags[50706]==bytes([1,2,0,0])
assert not any(tag in tags for tag in (50740,50741))  # No private data / original private container.
start=struct.unpack('<I',tags[273])[0]
raw=data[start:]
expected=b''.join(struct.pack('<H',(i*13+i//640)%1024) for i in range(640*480))
assert raw==expected
assert tags[50972]==hashlib.md5(raw).digest()
print('PASS independent TIFF tags, every uint16 sample and standard RawImageDigest MD5')

if shutil.which('exiftool'):
    result=json.loads(subprocess.check_output(['exiftool','-j','-DNGVersion','-ImageWidth','-ImageHeight','-Compression',str(p)]))[0]
    assert result['DNGVersion']=='1.2.0.0' and result['ImageWidth']==640 and result['ImageHeight']==480
    assert result['Compression']=='Uncompressed'
    print('PASS ExifTool identifies standard uncompressed DNG 1.2')

extra=ROOT/'build/dng-validation-python'
if extra.is_dir():sys.path.insert(0,str(extra))
try:
    import rawpy
    import tifffile
except ImportError:
    print('SKIP optional LibRaw/tifffile interoperability (not installed)')
else:
    with tifffile.TiffFile(p) as file:
        assert file.pages[0].asarray().tobytes()==expected
    with rawpy.imread(str(p)) as file:
        assert file.raw_image_visible.shape==(480,640)
        assert file.raw_image_visible.tobytes()==expected
        assert file.white_level==1023
    print('PASS TIFF decoder and LibRaw open DNG and recover every original sensor value')
