"""Actual C JPEG/MPO boundary and private original-archive integration checks."""
import ctypes
import hashlib
import io
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest

from PIL import Image, JpegImagePlugin

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import native_recovery as recovery
from camera_runtime_contract import CONTRACT_MATRIX, header


class Jpeg(ctypes.Structure):
    _fields_=[('offset',ctypes.c_size_t),('bytes',ctypes.c_size_t),('width',ctypes.c_uint),('height',ctypes.c_uint)]


class Media(ctypes.Structure):
    _fields_=[('count',ctypes.c_uint),('images',Jpeg*2)]


def jpeg(progressive=False,mode='RGB',size=(16,12),subsampling=None):
    out=io.BytesIO()
    color=(80,120,160) if mode=='RGB' else (10,20,30,40) if mode=='CMYK' else 100
    options={} if subsampling is None else dict(subsampling=subsampling)
    Image.new(mode,size,color).save(out,format='JPEG',progressive=progressive,**options)
    return out.getvalue()


def mpo(little=True, padding=0, secondary_attributes=False, secondary_index=False):
    a,b=jpeg(),jpeg(True)
    order='<' if little else '>'
    def tag(i,t,n,v):return struct.pack(order+'HHII',i,t,n,v)
    if secondary_attributes:
        attr=(b'II' if little else b'MM')+struct.pack(order+'HIH',42,8,1)
        attr+=tag(0xb001 if secondary_index else 0xb101,4,1,2)+struct.pack(order+'I',0)
        payload=b'MPF\0'+attr
        b=b[:2]+b'\xff\xe2'+struct.pack('>H',len(payload)+2)+payload+b[2:]
    tiff=(b'II' if little else b'MM')+struct.pack(order+'HI',42,8)
    tags=tag(0xb000,7,4,0x30303130)+tag(0xb001,4,1,2)+tag(0xb002,7,32,50)
    fixed=tiff+struct.pack(order+'H',3)+tags+struct.pack(order+'I',0)
    first_bytes=len(a)+2+2+4+len(fixed)+32+padding
    entries=struct.pack(order+'IIIHH',0x20020002,first_bytes,0,0,0)+struct.pack(order+'IIIHH',0x00020002,len(b),first_bytes-10,0,0)
    payload=b'MPF\0'+fixed+entries
    return a[:2]+b'\xff\xe2'+struct.pack('>H',len(payload)+2)+payload+a[2:]+bytes(padding)+b


class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory()
        lib=Path(cls.temp.name)/'native.so'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(ROOT/'include'),
                        str(ROOT/'source/storage/native_media.c'),str(ROOT/'source/storage/native_decode.c'),
                        '-ljpeg','-o',str(lib)],check=True)
        cls.lib=ctypes.CDLL(str(lib))
        cls.inspect=cls.lib.film_native_media_inspect
        cls.inspect.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(Media)]
        cls.inspect.restype=ctypes.c_int
        cls.decode=cls.lib.film_native_jpeg_decode
        cls.decode.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint,
                             ctypes.c_void_p,ctypes.c_size_t,ctypes.c_size_t,
                             ctypes.POINTER(ctypes.c_uint),ctypes.POINTER(ctypes.c_uint)]
        cls.decode.restype=ctypes.c_int

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def parse(self,payload,source=b'film-native-media-inspect/v1'):
        buffer=ctypes.create_string_buffer(payload)
        before=buffer.raw
        out=Media();out.count=99
        status=self.inspect(source,buffer,len(payload),ctypes.byref(out))
        self.assertEqual(buffer.raw,before)
        if status:self.assertEqual(bytes(out),bytes(Media()))
        return status,out

    def test_matrix_header_and_read_boundary(self):
        self.assertEqual((ROOT/'include/film_runtime_contract.h').read_text(),header())
        for source in ('film-native-media-inspect/v1',recovery.SOURCE):
            r=CONTRACT_MATRIX[source]
            self.assertFalse(r['compute'] or r['lane_admission'])
            self.assertIsNone(r['override'])
        self.assertFalse(CONTRACT_MATRIX['film-native-media-inspect/v1']['writes'])
        decode=CONTRACT_MATRIX['film-native-jpeg-decode/v1']
        self.assertTrue(decode['compute'])
        self.assertFalse(decode['lane_admission'] or decode['camera_calls'] or decode['writes'])
        self.assertIsNone(decode['override']);self.assertEqual(decode['proof'],'none')

    def test_frozen_legacy_header_does_not_admit_native_sources(self):
        legacy={k:v for k,v in CONTRACT_MATRIX.items() if not k.startswith('film-native-')}
        generated=header(legacy)
        for source in CONTRACT_MATRIX:
            if source.startswith('film-native-'):
                self.assertNotIn('!strcmp(source, "'+source+'")',generated)

    def test_actual_baseline_and_progressive_jpeg(self):
        for progressive in (False,True):
            data=jpeg(progressive)
            status,out=self.parse(data)
            self.assertEqual(status,0);self.assertEqual(out.count,1)
            self.assertEqual((out.images[0].offset,out.images[0].bytes,out.images[0].width,out.images[0].height),(0,len(data),16,12))

    def test_both_tiff_byte_orders_and_borrowed_members(self):
        for little in (False,True):
            data=mpo(little)
            status,out=self.parse(data)
            self.assertEqual(status,0);self.assertEqual(out.count,2)
            for member in out.images:
                with JpegImagePlugin.JpegImageFile(io.BytesIO(data[member.offset:member.offset+member.bytes])) as image:
                    image.load();self.assertEqual(image.size,(16,12))

    def test_unknown_source_scheduler_masquerade_and_wrong_registered_source(self):
        for source in (None,b'unknown',b'scheduler_decision:admitted',b'startup_warmup',b'film-darkroom/v1'):
            status,_=self.parse(jpeg(),source)
            self.assertEqual(status,6) # FILM_STATE

    def decoded(self,payload,index=0,source=b'film-native-jpeg-decode/v1'):
        status,media=self.parse(payload)
        self.assertEqual(status,0)
        member=media.images[index]
        stride=member.width*3+7
        capacity=stride*member.height
        pixels=ctypes.create_string_buffer(bytes([0xa7])*(capacity+16))
        encoded=ctypes.create_string_buffer(payload)
        w,h=ctypes.c_uint(99),ctypes.c_uint(99)
        rc=self.decode(source,encoded,len(payload),index,ctypes.byref(pixels,8),stride,capacity,ctypes.byref(w),ctypes.byref(h))
        self.assertEqual(encoded.raw[:len(payload)],payload)
        self.assertEqual(pixels.raw[:8],bytes([0xa7])*8)
        self.assertEqual(pixels.raw[capacity+8:capacity+16],bytes([0xa7])*8)
        if rc:
            self.assertEqual((w.value,h.value),(0,0))
            return rc,None
        self.assertEqual((w.value,h.value),(member.width,member.height))
        rows=[]
        for y in range(h.value):
            start=8+y*stride
            rows.append(pixels.raw[start:start+w.value*3])
            self.assertEqual(pixels.raw[start+w.value*3:start+stride],bytes([0xa7])*7)
        return rc,b''.join(rows)

    def test_actual_C_decode_baseline_progressive_grayscale_and_stereo(self):
        cases=[(jpeg(),0),(jpeg(True),0),(jpeg(mode='L'),0),
               (mpo(secondary_attributes=True,padding=31),0),(mpo(secondary_attributes=True,padding=31),1)]
        for payload,index in cases:
            status,media=self.parse(payload);self.assertEqual(status,0)
            member=media.images[index]
            with JpegImagePlugin.JpegImageFile(io.BytesIO(payload[member.offset:member.offset+member.bytes])) as image:
                expected=image.convert('RGB').tobytes()
            rc,pixels=self.decoded(payload,index)
            self.assertEqual(rc,0)
            self.assertLessEqual(max(abs(a-b) for a,b in zip(pixels,expected)),2)
            self.assertEqual(self.decoded(payload,index)[1],pixels)

    def test_C_decode_rejects_wrong_sources_cmyk_and_missing_quantization(self):
        for source in (None,b'unknown',b'scheduler_decision:admitted',b'startup_warmup',b'film-native-media-inspect/v1'):
            self.assertEqual(self.decoded(jpeg(),source=source)[0],6)
        self.assertEqual(self.decoded(jpeg(mode='CMYK'))[0],2)
        original=jpeg();pos=2;parts=[original[:2]]
        while original[pos+1]!=0xda:
            size=int.from_bytes(original[pos+2:pos+4],'big')+2
            if original[pos+1]!=0xdb:parts.append(original[pos:pos+size])
            pos+=size
        parts.append(original[pos:])
        self.assertEqual(self.decoded(b''.join(parts))[0],5)

    def test_C_decode_capacity_index_overlap_and_pixel_budget(self):
        payload=jpeg();encoded=ctypes.create_string_buffer(payload)
        pixels=ctypes.create_string_buffer(16*12*3)
        w,h=ctypes.c_uint(99),ctypes.c_uint(99)
        def call(index=0,dest=pixels,stride=48,capacity=16*12*3,wp=None,hp=None,data=encoded,length=len(payload)):
            return self.decode(b'film-native-jpeg-decode/v1',data,length,index,dest,stride,capacity,
                               wp if wp is not None else ctypes.byref(w),hp if hp is not None else ctypes.byref(h))
        self.assertEqual(call(index=1),1)
        self.assertEqual(call(stride=47),1)
        self.assertEqual(call(capacity=16*12*3-1),1)
        self.assertEqual(call(dest=encoded),1)
        self.assertEqual(encoded.raw[:len(payload)],payload)
        self.assertEqual(call(wp=ctypes.cast(encoded,ctypes.POINTER(ctypes.c_uint))),1)
        self.assertEqual(encoded.raw[:len(payload)],payload)
        self.assertEqual(call(hp=ctypes.byref(w)),1)
        large=jpeg(size=(1025,768));buffer=ctypes.create_string_buffer(large)
        self.assertEqual(call(data=buffer,length=len(large)),2)
        self.assertEqual((w.value,h.value),(0,0))

    def test_C_decode_virtual_array_budget_has_no_disk_fallback(self):
        # A real maximum-size 4:4:4 progressive image needs more coefficient
        # storage than the registered virtual-array budget, even though its
        # compressed file and output dimensions are otherwise in range.
        rc,_=self.decoded(jpeg(True,size=(1024,768),subsampling=0))
        self.assertEqual(rc,4) # FILM_NOMEM, not a repaired/partial success.

    def test_truncated_trailing_and_concatenated_unindexed_jpeg(self):
        data=jpeg()
        for bad in (b'',b'\xff\xd8',data[:-1],data+b'junk',data+data):
            self.assertNotEqual(self.parse(bad)[0],0)
        data=mpo()
        for length in range(len(data)):
            self.assertNotEqual(self.parse(data[:length])[0],0)

    def test_malformed_mpf_never_downgrades_to_single_jpeg(self):
        data=mpo()
        # TIFF magic, IFD outside segment, count, MP-entry offset, first origin,
        # overlapping/outside second member, wrong size, non-JPEG format.
        for offset,value,width in [(12,0,2),(14,0xffffffff,4),(40,3,4),(52,0xffffffff,4),
                                   (68,1,4),(84,1,4),(84,0xffffffff,4),(80,1,4),(76,0x01000000,4)]:
            bad=bytearray(data)
            bad[offset:offset+width]=value.to_bytes(width,'little')
            self.assertNotEqual(self.parse(bytes(bad))[0],0,(offset,value))

    def test_native_style_padding_and_secondary_attribute_ifd(self):
        for little in (False,True):
            for padding in (0,31,63):
                data=mpo(little,padding,secondary_attributes=True)
                status,out=self.parse(data)
                self.assertEqual(status,0)
                first=out.images[0]
                self.assertEqual(data[first.bytes-2:first.bytes],b'\xff\xd9')
                self.assertEqual(out.images[1].offset,first.bytes+padding)
                if padding:
                    bad=bytearray(data);bad[first.bytes]=1
                    self.assertNotEqual(self.parse(bytes(bad))[0],0)
        self.assertNotEqual(self.parse(mpo(padding=64))[0],0)
        self.assertNotEqual(self.parse(mpo(secondary_attributes=True,secondary_index=True))[0],0)
        bad=bytearray(mpo());bad[56:60]=(8).to_bytes(4,'little')
        self.assertNotEqual(self.parse(bytes(bad))[0],0) # IFD cycle.

    def test_null_and_resource_limits(self):
        out=Media();out.count=99
        self.assertEqual(self.inspect(b'film-native-media-inspect/v1',None,0,ctypes.byref(out)),1)
        data=ctypes.create_string_buffer(jpeg())
        self.assertEqual(self.inspect(b'film-native-media-inspect/v1',data,16*1024*1024+1,ctypes.byref(out)),1)
        self.assertEqual(self.inspect(b'film-native-media-inspect/v1',data,len(data)-1,None),1)
        self.assertEqual(out.count,0)

    def test_bounded_mutation_scan(self):
        rng=random.Random(164)
        for _ in range(1000):
            data=bytearray(mpo(bool(rng.randrange(2))))
            for _ in range(rng.randrange(1,6)):
                data[rng.randrange(len(data))]=rng.randrange(256)
            status,out=self.parse(bytes(data))
            self.assertIn(status,(0,5)) # FILM_CORRUPT; structural acceptance does not certify entropy.
            if status==0:
                self.assertEqual(out.count,2)
                self.assertLessEqual(out.images[1].offset+out.images[1].bytes,len(data))

    def test_original_romfs_mpo(self):
        cxi=Path('/Users/a64/Desktop/0004001000020400.0000001e Nintendo 3DS Camera (CTR-N-HEPJ) (J).cxi')
        if not cxi.exists():self.skipTest('private CXI fixture not present')
        data=cxi.read_bytes();self.assertEqual(hashlib.sha256(data).hexdigest(),recovery.CXI_SHA)
        lv3=0x298000
        _,_,_,_,_,_,_,fm,fms,base=struct.unpack_from('<10I',data,lv3)
        off=0;sample=None
        while off<fms:
            _,_,start,size,_,n=struct.unpack_from('<IIQQII',data,lv3+fm+off)
            name=data[lv3+fm+off+32:lv3+fm+off+32+n].decode('utf-16-le')
            if name=='resizable.mpo':sample=data[lv3+base+start:lv3+base+start+size];break
            off=(off+32+n+3)&~3
        self.assertIsNotNone(sample)
        status,out=self.parse(sample)
        self.assertEqual(status,0);self.assertEqual(out.count,2)
        for member in out.images:
            with JpegImagePlugin.JpegImageFile(io.BytesIO(sample[member.offset:member.offset+member.bytes])) as image:
                image.load();self.assertEqual(image.size,(member.width,member.height))
        for index in (0,1):
            rc,pixels=self.decoded(sample,index)
            self.assertEqual(rc,0)
            member=out.images[index]
            with JpegImagePlugin.JpegImageFile(io.BytesIO(sample[member.offset:member.offset+member.bytes])) as image:
                expected=image.convert('RGB').tobytes()
            self.assertLessEqual(max(abs(a-b) for a,b in zip(pixels,expected)),2)
        print('Private original RomFS MPO:',len(sample),'bytes;',out.images[0].width,'x',out.images[0].height)


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.directory=Path(self.temp.name)/'archive'
        self.cxi=Path('/Users/a64/Desktop/0004001000020400.0000001e Nintendo 3DS Camera (CTR-N-HEPJ) (J).cxi')
        self.code=ROOT/'build/hardware/native-trace/cxi-jp/exefs/code.bin'
        if not self.cxi.exists() or not self.code.exists():self.skipTest('private original fixtures not present')

    def tearDown(self):self.temp.cleanup()

    def archive(self):return recovery.create(self.cxi,self.code,self.directory)

    def rewrite(self,m):
        data=(json.dumps(m,indent=2)+'\n').encode()
        (self.directory/'manifest.json').write_bytes(data)
        (self.directory/'manifest.sha256').write_text(hashlib.sha256(data).hexdigest()+'\n')

    def test_actual_archive_verified_and_no_overwrite(self):
        m=self.archive();self.assertEqual(recovery.verify(self.directory),m)
        self.assertFalse(m['device_rollback_tested']);self.assertEqual(m['scope'],'program_content_only')
        with self.assertRaises(FileExistsError):self.archive()
        self.assertEqual(recovery.verify(self.directory),m)

    def test_unknown_source_and_wrong_version_create_nothing(self):
        with self.assertRaises(ValueError):recovery.create(self.cxi,self.code,self.directory,'scheduler_decision:admitted')
        self.assertFalse(self.directory.exists())
        bad=Path(self.temp.name)/'bad-code';bad.write_bytes(b'wrong title')
        with self.assertRaises(ValueError):recovery.create(self.cxi,bad,self.directory)
        self.assertFalse(self.directory.exists())

    def test_missing_misplaced_or_fake_local_proof(self):
        m=self.archive()
        for change in ('missing','misplaced','fake'):
            bad=json.loads(json.dumps(m))
            if change=='missing':bad.pop('local_trace')
            if change=='misplaced':bad['metadata']=bad.pop('local_trace')
            if change=='fake':bad['local_trace']['source']='scheduler_decision:admitted'
            self.rewrite(bad)
            with self.assertRaises(ValueError):recovery.verify(self.directory)

    def test_corrupt_extra_symlink_and_traversal_rejected(self):
        m=self.archive();code=self.directory/'original-code.bin';original=code.read_bytes()
        code.write_bytes(b'changed')
        with self.assertRaises(ValueError):recovery.verify(self.directory)
        code.write_bytes(original)
        extra=self.directory/'extra';extra.write_text('unexpected')
        with self.assertRaises(ValueError):recovery.verify(self.directory)
        extra.unlink();code.unlink();code.symlink_to(self.code)
        with self.assertRaises(ValueError):recovery.verify(self.directory)
        code.unlink();code.write_bytes(original)
        bad=json.loads(json.dumps(m));bad['files']['../outside']=bad['files'].pop('original-code.bin')
        self.rewrite(bad)
        with self.assertRaises(ValueError):recovery.verify(self.directory)


if __name__=='__main__':unittest.main()
