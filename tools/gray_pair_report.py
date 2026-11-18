"""Read opt-in Film3DS gray calibration evidence; never modifies originals.

Usage:
 python3 tools/gray_pair_report.py --dng ROLL0001.DNG --yuv ROLL0001.F3Y \
   --calibration romfs/calibration/display-gray.f3isp --output pair.json
 Optional matched 21-cell rectangles (left,top,right,bottom) from one fixed chart:
   --preview-roi 0 210 640 280 --raw-roi 0 210 640 280

The F3Y is a last-completed pre-shutter preview, NOT a simultaneous RAW frame.
The per-cell Bayer approximation is for QA, not a replacement for production
FilmRawScene demosaic / spatial ISP. Use independent hold-out captures to fit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib
import numpy as np

W,H=640,480
FRAME=W*H*2
HEADER=64

def read_f3y(path):
    content=Path(path).read_bytes()
    if len(content)<HEADER or content[:4]!=b"F3YG":
        raise ValueError("F3Y: invalid length or magic")
    version=struct.unpack_from("<I",content,4)[0]
    header_bytes={1:64,2:80}.get(version)
    if header_bytes is None or len(content)!=header_bytes+FRAME:
        raise ValueError("F3Y: invalid version or exact length")
    hdr=content[:header_bytes]
    meta=struct.unpack_from("<14I",hdr,4)
    ver,id_,raw_crc,width,height,yuv,level,mode,ae,awb,time_low,time_high,length,payload_crc=meta
    if (ver not in (1,2) or width!=W or height!=H or yuv>3 or mode>1 or
        (level if level<0x80000000 else level-0x100000000) not in range(-3,4) or
        ae>1 or awb>1 or (time_low==0 and time_high==0) or length!=FRAME):
        raise ValueError("F3Y: invalid metadata")
    if version==2:
        region=struct.unpack_from("<4I",hdr,60)
        x,y,width,height=region
        if (width<80 or height<60 or width>W or height>H or
            width*3!=height*4 or x%2 or y%2 or x+width>W or y+height>H):
            raise ValueError("F3Y: invalid AE window")
        crc_offset=76
    else:
        region=None # v1 never recorded the physical AE window.
        crc_offset=60
    if zlib.crc32(hdr[:crc_offset])!=struct.unpack_from("<I",hdr,crc_offset)[0]:
        raise ValueError("F3Y: header CRC mismatch")
    pixels=content[header_bytes:]
    if zlib.crc32(pixels)!=payload_crc:
        raise ValueError("F3Y: preview payload CRC mismatch")
    return dict(id=id_,source_crc=raw_crc,version=version,ae_window=region,
                native_level=struct.unpack_from("<i",hdr,28)[0],
                mode=mode,auto_exposure=bool(ae),auto_white_balance=bool(awb),
                yuv=yuv,preview_time_ms=time_low+(time_high<<32)),pixels

def read_dng(path):
    data=Path(path).read_bytes()
    if len(data)!=2048+FRAME or data[:4]!=b"II*\0" or struct.unpack_from("<I",data,4)[0]!=8:
        raise ValueError("DNG: unexpected native RAW layout")
    count=struct.unpack_from("<H",data,8)[0]
    if count<10 or count>40:
        raise ValueError("DNG: invalid IFD count")
    tags={}
    for i in range(count):
        tag,kind,n,loc=struct.unpack_from("<HHII",data,10+i*12)
        tags[tag]=(kind,n,loc)
    required={256:W,257:H,258:16,259:1,262:32803,273:2048,277:1,278:H,279:FRAME}
    for key,value in required.items():
        if tags.get(key,(0,0,0))[2]!=value:
            # SHORT fields are stored in the low halfword (same for these values).
            raise ValueError(f"DNG: unsupported TIFF tag {key}")
    if tags.get(50972,(0,0,0))[:2]!=(1,16):
        raise ValueError("DNG: missing RawImageDigest")
    digest_offset=tags[50972][2]
    if digest_offset<512 or digest_offset>2032:
        raise ValueError("DNG: invalid digest pointer")
    rawwire=data[2048:]
    if hashlib.md5(rawwire).digest()!=data[digest_offset:digest_offset+16]:
        raise ValueError("DNG: failed original MD5 digest")
    raw=np.frombuffer(rawwire,dtype="<u2").reshape((H,W))
    if np.max(raw)>1023:
        raise ValueError("DNG: invalid RAW10 value")
    packed=np.empty((H,W,2),dtype=np.uint8)
    packed[:,:,0]=(raw>>2).astype(np.uint8)
    packed[:,:,1]=(raw&3).astype(np.uint8)
    crc=zlib.crc32(struct.pack("<III",W,H,4))
    crc=zlib.crc32(packed.tobytes(),crc)
    return raw,crc

def read_gray_profile(path):
    d={}
    for line in Path(path).read_text().splitlines():
        parts=line.split()
        if parts and parts[0]!="F3ISP1":
            d[parts[0]]=[float(x) for x in parts[1:]]
    for key,count in (("black",4),("white",1),("wb",3),("ccm",9),
                      ("ccm_offset",3),("shading_r",6),("shading_g",6),("shading_b",6)):
        if len(d.get(key,[]))!=count:
            raise ValueError("bad Display gray calibration: "+key)
    return d

def percentiles(a):
    return [round(float(v),7) for v in np.percentile(a,[1,10,50,90,99])]

def linear_preview(packed, yuv):
    a=np.frombuffer(packed,dtype=np.uint8).reshape(H,W//2,4)
    ys=np.empty((H,W),dtype=np.float32)
    ys[:,::2]=a[:,:,0];ys[:,1::2]=a[:,:,2]
    if yuv in (1,3): code=np.clip((ys-16)/219,0,1)
    else: code=ys/255.
    return np.where(code<=.04045,code/12.92,((code+.055)/1.055)**2.4)

def region_stats(preview,raw,roi_p,roi_r,profile):
    def rect(r):
        if r is None or len(r)!=4:return None
        x0,y0,x1,y1=map(int,r)
        if not (0<=x0<x1<=W and 0<=y0<y1<=H):raise ValueError("invalid region")
        return x0,y0,x1,y1
    rp,rr=rect(roi_p),rect(roi_r)
    if rp is None or rr is None:return []
    black=np.asarray(profile["black"],dtype=float)
    white=profile["white"][0]
    wb=np.asarray(profile["wb"],dtype=float)
    ccm=np.asarray(profile["ccm"],dtype=float).reshape(3,3)
    offset=np.asarray(profile["ccm_offset"],dtype=float)
    shade=np.asarray([profile["shading_r"],profile["shading_g"],profile["shading_b"]])
    rows=[]
    for i in range(21):
        def area(r):
            x0,y0,x1,y1=r
            lo=x0+(x1-x0)*i//21;hi=x0+(x1-x0)*(i+1)//21
            # 30%-70% of the bar horizontally and middle 40% vertically.
            left=lo+(hi-lo)*3//10;right=lo+(hi-lo)*7//10
            top=y0+(y1-y0)*3//10;bottom=y0+(y1-y0)*7//10
            return left,top,max(left+1,right),max(top+1,bottom)
        pl,pt,pr,pb=area(rp);rl,rt,rr_,rb=area(rr)
        # Even RGGB alignment: four phase samples must be present.
        rl+=(rl&1);rt+=(rt&1);rr_-=(rr_&1);rb-=(rb&1)
        if rr_<=rl or rb<=rt:raise ValueError("RAW chart bars too narrow")
        p=np.median(preview[pt:pb,pl:pr])
        a=raw[rt:rb,rl:rr_]
        phase=np.array([np.median(a[0::2,0::2]),np.median(a[0::2,1::2]),
                        np.median(a[1::2,0::2]),np.median(a[1::2,1::2])])
        unit=np.maximum((phase-black)/(white-black),0)
        rgb=np.array([unit[0],(unit[1]+unit[2])/2,unit[3]])*wb
        x=2*((rl+rr_)/2)/(W-1)-1;y=2*((rt+rb)/2)/(H-1)-1
        basis=np.array([1,x,y,x*x,x*y,y*y])
        rgb*=2**np.clip(shade@basis,-2,2)
        rgb=ccm@rgb+offset
        native=float(p);gray=float(np.array([.2126,.7152,.0722])@rgb)
        rows.append(dict(patch=i,preview_linear=native,display_gray_linear=gray,
                         raw_sensor_mean=float(np.mean(phase)),
                         ev_residual=None if min(native,gray)<=.0001 else
                         float(np.log2(native/gray)),
                         raw_clip_fraction=float(np.mean(a>=1023))))
    return rows

def analyze(dng,f3y,calibration,roi_p=None,roi_r=None):
    meta,yuv=read_f3y(f3y)
    raw,crc=read_dng(dng)
    if crc!=meta["source_crc"]:
        raise ValueError("DNG/F3Y: source CRC mismatch")
    profile=read_gray_profile(calibration)
    preview=linear_preview(yuv,meta["yuv"])
    result={"record":meta,"original_md5":hashlib.md5(Path(dng).read_bytes()).hexdigest(),
            "preview_linear_p01_p10_p50_p90_p99":percentiles(preview),
            "raw10_p01_p10_p50_p90_p99":percentiles(raw),
            "native_preview_clipped_fraction":float(np.mean(preview>=.98)),
            "raw10_clipped_fraction":float(np.mean(raw>=1023)),
            "patches":region_stats(preview,raw,roi_p,roi_r,profile),
            "note":"YUV and RAW are successive, not simultaneous; patch RAW ISP is flat-zone approximation"}
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ("dng","yuv","calibration"):
        p.add_argument("--"+n,required=True,type=Path)
    p.add_argument("--output",type=Path)
    p.add_argument("--preview-roi",nargs=4,type=int)
    p.add_argument("--raw-roi",nargs=4,type=int)
    a=p.parse_args()
    result=analyze(a.dng,a.yuv,a.calibration,a.preview_roi,a.raw_roi)
    text=json.dumps(result,indent=2,ensure_ascii=False)
    if a.output:
        a.output.write_text(text+"\n")
    else:
        print(text)
if __name__=="__main__":main()
