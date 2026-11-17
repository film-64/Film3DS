"""Offline screen-chart calibration through the real C RAW ISP (NumPy/Pillow).

Layout JSON explicitly supplies color/raw_color/gray/raw_gray quadrilateral
corners TL,TR,BR,BL. Reference images must have an sRGB marker or an ICC profile.
This is a calibration of this display/camera/setup, not a measured sensor gamut.
"""
from pathlib import Path
import argparse
import hashlib
import io
import json
import struct
import subprocess
import time
import zlib
import numpy as np
from PIL import Image, ImageCms, ImageDraw
from camera_runtime_contract import CONTRACT_MATRIX

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'film-display-chart-calibrate/v1'


def decode(x):
    x = np.maximum(x, 0)
    return np.where(x <= .04045, x / 12.92, ((x + .055) / 1.055) ** 2.4)


def encode(x):
    x = np.maximum(x, 0)
    return np.where(x <= .0031308, x * 12.92, 1.055 * x ** (1 / 2.4) - .055)


def read_raw(path):
    # Bounded before allocation; verify original record CRC and every low byte.
    with Path(path).open('rb') as f:
        b = f.read(614705)
    if len(b) != 614704 or b[:4] != b'F3SR':
        raise ValueError('expected one legacy 640x480 RAW F3S record')
    version, header, size, w, h, fmt = struct.unpack_from('<6I', b, 4)
    if (version, header, size, w, h, fmt) != (1, 304, 614400, 640, 480, 4):
        raise ValueError('unsupported F3S layout')
    crc = zlib.crc32(b[304:], zlib.crc32(b[:300]))
    if crc != struct.unpack_from('<I', b, 300)[0]:
        raise ValueError('F3S CRC mismatch')
    wire = np.frombuffer(b[304:], np.uint8).reshape(h, w, 2)
    if (wire[..., 1] > 3).any():
        raise ValueError('invalid RAW10 low byte')
    return b[304:], (wire[..., 0].astype(np.uint16) * 4 + wire[..., 1])


def bad_pixels(raw):
    # Conservative isolated hot/dead candidate detection, separately per phase.
    # The emitted explicit list is scoped to this capture, never flashed to camera.
    found = []
    for py in range(2):
        for px in range(2):
            a = raw[py::2, px::2].astype(float)
            padded = np.pad(a, 2, mode='reflect')
            neighbors = np.stack([padded[2+dy:2+dy+a.shape[0], 2+dx:2+dx+a.shape[1]]
                                  for dy in range(-2,3) for dx in range(-2,3) if dx or dy])
            med = np.median(neighbors, axis=0)
            spread = np.median(np.abs(neighbors-med),axis=0)
            mask = (np.abs(a-med) > np.maximum(80, 8*spread))
            mask[:2] = False; mask[-2:] = False; mask[:, :2] = False; mask[:, -2:] = False
            candidates=set(map(tuple,np.argwhere(mask)))
            while candidates:
                seed=candidates.pop();component={seed};pending=[seed]
                while pending:
                    yy,xx=pending.pop()
                    for dy in (-1,0,1):
                        for dx in (-1,0,1):
                            neighbor=(yy+dy,xx+dx)
                            if neighbor in candidates:
                                candidates.remove(neighbor);component.add(neighbor);pending.append(neighbor)
                # Larger components may be real chart detail, not isolated defects.
                # At most eight also guarantees an unmasked repair neighbor.
                if len(component)<=8:
                    found.extend((int(x*2+px),int(y*2+py)) for y,x in component)
    if len(found) > 4096:
        raise ValueError('too many defect candidates; inspect layout/RAW')
    return sorted(found, key=lambda xy: (xy[1], xy[0]))


def reference(path):
    with Image.open(path) as im:
        if im.info.get('icc_profile'):
            im = ImageCms.profileToProfile(im, ImageCms.ImageCmsProfile(io.BytesIO(im.info['icc_profile'])),
                                           ImageCms.createProfile('sRGB'), outputMode='RGB')
        elif 'srgb' not in im.info:
            raise ValueError('reference has no ICC or sRGB marker')
        return np.asarray(im.convert('RGB'), dtype=float)/255


def patch_samples(image, corners, rows, cols):
    corners = np.asarray(corners, dtype=float)
    if corners.shape != (4, 2) or not np.isfinite(corners).all():
        raise ValueError('four finite corners required')
    # Exact projective mapping, rather than axis-aligned chart boxes.
    src = [(0, 0), (1, 0), (1, 1), (0, 1)]
    a, b = [], []
    for (u, v), (x, y) in zip(src, corners):
        a.extend([[u,v,1,0,0,0,-u*x,-v*x], [0,0,0,u,v,1,-u*y,-v*y]])
        b.extend([x,y])
    h = np.append(np.linalg.solve(a, b), 1).reshape(3,3)
    patches, centers = [], []
    for row in range(rows):
        for col in range(cols):
            u, v = np.meshgrid((col+np.linspace(.3,.7,13))/cols, (row+np.linspace(.3,.7,13))/rows)
            q = np.stack([u.ravel(),v.ravel(),np.ones(u.size)])
            q = h @ q; xy = (q[:2]/q[2]).T
            if (xy < 0).any() or (xy[:,0] >= image.shape[1]).any() or (xy[:,1] >= image.shape[0]).any():
                raise ValueError('chart sample outside image')
            x, y = np.rint(xy).astype(int).T
            patches.append(np.median(image[y,x], axis=0)); centers.append(xy.mean(axis=0))
    return np.array(patches), np.array(centers)


def lab(rgb):
    xyz = np.maximum(rgb, 0) @ np.array([[.4124564,.2126729,.0193339],
                                      [.3575761,.7151522,.1191920],[.1804375,.0721750,.9503041]])
    t = xyz / np.array([.95047,1,1.08883])
    f = np.where(t > (6/29)**3, np.cbrt(t), t/(3*(6/29)**2)+4/29)
    return np.stack([116*f[...,1]-16,500*(f[...,0]-f[...,1]),200*(f[...,1]-f[...,2])],axis=-1)


def fit(x, y):
    design = np.column_stack([x, np.ones(len(x))])
    # Penalize excessive cross-channel gains with a small identity prior.
    prior = np.vstack([np.eye(3), np.zeros(3)])
    penalty = np.diag([.001,.001,.001,0])
    return np.linalg.solve(design.T@design+penalty, design.T@y+penalty@prior)


def errors(x, y):
    de = np.linalg.norm(lab(x)-lab(y),axis=1)
    return dict(mean_deltaE76=float(de.mean()),max_deltaE76=float(de.max()),per_patch_deltaE76=de.tolist())


def run_isp(out, name, wire, bad, matrix=None, local=0, black=None, shading=None):
    config = ['F3RAW1','width 640','height 480','cfa 0','white 1023','shoulder 0',
              'grain_amount 0','local_strength '+str(local),'local_limit_ev 1.5']
    if black is not None: config += ['black '+' '.join(map(str,black))]
    if shading is not None:
        config += [key+' '+' '.join(map(str,row)) for key,row in zip(('shading_r','shading_g','shading_b'),shading)]
    if matrix is not None:
        config += ['ccm '+' '.join(map(str,matrix[:3].T.ravel())), 'ccm_offset '+' '.join(map(str,matrix[3]))]
    config += [f'bad_pixel {x} {y}' for x,y in bad]
    params = out/(name+'.params.txt'); params.write_text('\n'.join(config)+'\n')
    rawpath = out/(name.split('-')[0]+'.bin')
    if not rawpath.exists(): rawpath.write_bytes(wire)
    start = time.perf_counter()
    result = subprocess.run([str(ROOT/'build/filmraw'),str(rawpath),str(params),str(out/name),str(ROOT/'romfs/grain/medium.f3g')],
                            capture_output=True,text=True)
    elapsed = time.perf_counter()-start
    if result.returncode: raise RuntimeError(result.stderr)
    encoded = np.fromfile(out/name/'prepared.rgbf32', dtype=np.float32).reshape(480,640,3)
    linear = decode(encoded)
    np.save(out/(name+'-linear-srgb.npy'),linear)
    image = Image.fromarray(np.uint8(np.rint(np.clip(encode(linear),0,1)*255)))
    image.save(out/(name+'.png'),icc_profile=ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes())
    return linear, elapsed


def tiff16(path, encoded, icc):
    """One uncompressed RGB16 TIFF strip, little endian, with an ICC tag."""
    pixels=np.asarray(np.rint(np.clip(encoded,0,1)*65535),dtype='<u2')
    h,w,_=pixels.shape; n=11; bits_offset=8+2+12*n+4;icc_offset=bits_offset+6
    pixel_offset=icc_offset+len(icc);pixel_offset+=(pixel_offset%2)
    tags=[(256,4,1,w),(257,4,1,h),(258,3,3,bits_offset),(259,3,1,1),
          (262,3,1,2),(273,4,1,pixel_offset),(277,3,1,3),(278,4,1,h),
          (279,4,1,pixels.nbytes),(284,3,1,1),(34675,7,len(icc),icc_offset)]
    with path.open('xb') as f:
        f.write(struct.pack('<2sHIH',b'II',42,8,n))
        for tag,kind,count,value in tags:f.write(struct.pack('<HHII',tag,kind,count,value))
        f.write(struct.pack('<I3H',0,16,16,16));f.write(icc)
        if f.tell()%2:f.write(b'\0')
        f.write(pixels.tobytes())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('color-raw','gray-raw','color-reference','gray-reference','layout','output'):
        p.add_argument('--'+key, type=Path, required=True)
    p.add_argument('--p3-profile',type=Path,help='optional standard Display P3 ICC output profile')
    args = p.parse_args()
    p3_icc=None
    if args.p3_profile:
        profile=ImageCms.getOpenProfile(str(args.p3_profile))
        if 'display p3' not in ImageCms.getProfileDescription(profile).lower(): raise ValueError('expected Display P3 ICC')
        p3_icc=profile.tobytes()
    if SOURCE not in CONTRACT_MATRIX: raise ValueError('unregistered compute source')
    layout = json.loads(args.layout.read_text())
    color_wire, color_raw = read_raw(args.color_raw); gray_wire, gray_raw = read_raw(args.gray_raw)
    cref, gref = reference(args.color_reference), reference(args.gray_reference)
    target, _ = patch_samples(decode(cref), layout['color'],4,6)
    gray_target, _ = patch_samples(decode(gref), layout['gray'],1,21)
    args.output.mkdir()  # never overwrite previous calibration
    out = args.output
    report = dict(source=SOURCE,local_trace=dict(entry='explicit_host_CLI',source=SOURCE),
                  input_sha256={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in
                                (args.color_raw,args.gray_raw,args.color_reference,args.gray_reference,args.layout)},
                  reference_space='sRGB tags/ICC converted to sRGB; P3 display is not a P3 file assignment',
                  calibration_certified=False,layout=layout,host_seconds={})
    # Darkest displayed patch estimates this setup's black/flare pedestal, not
    # optical-black sensor calibration. Keep 10% margin for noise/flare variation.
    _, gray_centers = patch_samples(gray_raw[...,None],layout['raw_gray'],1,21)
    cx,cy = np.rint(gray_centers[0]).astype(int)
    black=[]
    for py in range(2):
        for px in range(2):
            y0=max(0,cy-14);x0=max(0,cx-8)
            y0+=(py-y0)%2;x0+=(px-x0)%2
            black.append(float(np.median(gray_raw[y0:cy+14:2,x0:cx+8:2]))*.9)
    report['display_flare_black_estimate']=black
    bad = {}; images = {}
    for name, wire, raw in [('color',color_wire,color_raw),('gray',gray_wire,gray_raw)]:
        bad[name] = bad_pixels(raw)
        report[name] = dict(bad_pixel_candidates=len(bad[name]),saturated_samples=int((raw>=1023).sum()),
                            raw_percentiles=np.percentile(raw,[0,1,50,99,100]).tolist())
        images[name], report['host_seconds'][name+'-baseline'] = run_isp(out,name+'-baseline',wire,[])
        images[name+'-clean'], report['host_seconds'][name+'-clean'] = run_isp(out,name+'-clean',wire,bad[name],black=black)
    # Fit log2 white response on user-declared blank background rectangles.
    points=[]; whites=[]
    for x0,y0,x1,y1 in layout['white_regions']:
        for y in range(y0,y1-15,16):
            for x in range(x0,x1-15,16):
                points.append((2*(x+7.5)/639-1,2*(y+7.5)/479-1))
                whites.append(np.median(images['gray-clean'][y:y+16,x:x+16],axis=(0,1)))
    xy=np.array(points); xx,yy=xy.T
    basis=np.stack([np.ones(len(xx)),xx,yy,xx*xx,xx*yy,yy*yy],axis=1)
    values=np.log2(np.maximum(whites,1e-6)); keep=np.ones(len(xx),bool)
    for _ in range(4):  # fixed robust regression passes, no runtime retry/recapture
        coeff=np.linalg.lstsq(basis[keep],values[keep],rcond=None)[0]
        residual=np.linalg.norm(basis@coeff-values,axis=1)
        keep=residual<=np.percentile(residual,90)
    shading=-coeff.T;shading[:,0]=0
    if not np.isfinite(shading).all() or (np.abs(shading)>4).any(): raise ValueError('invalid shading fit')
    report['lens_shading']=shading.tolist()
    for name,wire in [('color',color_wire),('gray',gray_wire)]:
        images[name+'-flat'],report['host_seconds'][name+'-flat']=run_isp(out,name+'-flat',wire,bad[name],black=black,shading=shading)
    samples, centers = patch_samples(images['color-flat'],layout['raw_color'],4,6)
    matrix = fit(samples,target)
    if (np.abs(matrix[:3])>8).any() or (np.abs(matrix[3])>1).any(): raise ValueError('calibration exceeds ISP contract')
    prediction = np.column_stack([samples,np.ones(24)])@matrix
    loo = []
    for i in range(24):
        keep = np.arange(24)!=i
        loo.append(np.append(samples[i],1)@fit(samples[keep],target[keep]))
    report['color_fit'] = dict(baseline=errors(patch_samples(images['color'],layout['raw_color'],4,6)[0],target), flat_uncorrected=errors(samples,target), fitted=errors(prediction,target),
                              leave_one_out=errors(np.array(loo),target), matrix=matrix.tolist())
    # The gray capture has a different exposure. Calibrate its neutral endpoints
    # separately; never claim the color-shot exposure transfers to another frame.
    gray_flat,_=patch_samples(images['gray-flat'],layout['raw_gray'],1,21)
    gray_gain=(gray_target[-1]-gray_target[0])/(gray_flat[-1]-gray_flat[0])
    gray_matrix=np.vstack([np.diag(gray_gain),gray_target[0]-gray_flat[0]*gray_gain])
    report['gray_capture_matrix']=gray_matrix.tolist()
    for name, wire in [('color',color_wire),('gray',gray_wire)]:
        active_matrix=matrix if name=='color' else gray_matrix
        for variant, local in [('calibrated',0),('local',.35)]:
            key=name+'-'+variant
            images[key],report['host_seconds'][key]=run_isp(out,key,wire,bad[name],active_matrix,local,black=black,shading=shading)
    srgb_icc=ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
    # Linear sRGB -> XYZ D65 -> Display P3 D65. Transfer curves are the same,
    # primaries are not. Embed the actual destination ICC, never merely relabel.
    srgb_xyz=np.array([[.4123907993,.3575843394,.1804807884],[.2126390059,.7151686788,.0721923154],[.0193308187,.1191947798,.9505321522]])
    p3_xyz=np.array([[.4865709486,.2656676932,.1982172852],[.2289745641,.6917385218,.0792869141],[0,.0451133819,1.0439443689]])
    for name in ('color-calibrated','color-local','gray-calibrated','gray-local'):
        tiff16(out/(name+'-srgb16.tif'),encode(images[name]),srgb_icc)
        if p3_icc:
            p3=images[name]@srgb_xyz.T@np.linalg.inv(p3_xyz).T
            Image.fromarray(np.uint8(np.rint(np.clip(encode(p3),0,1)*255))).save(out/(name+'-display-p3.png'),icc_profile=p3_icc)
            tiff16(out/(name+'-display-p3-16.tif'),encode(p3),p3_icc)
    gray_samples, _ = patch_samples(images['gray-calibrated'],layout['raw_gray'],1,21)
    measured = gray_samples@np.array([.2126,.7152,.0722])
    target_y = gray_target@np.array([.2126,.7152,.0722])
    report['gray'].update(measured_linear_y=measured.tolist(),reference_linear_y=target_y.tolist(),
                          monotonic_steps=int((np.diff(measured)>0).sum()),total_steps=20,
                          note='Separate capture; no asserted sensor dynamic range in stops from an 8bit screen ramp')
    # Labeled overview and sampling overlay make the fit reviewable.
    canvas = Image.new('RGB',(640*3,520*2),'white'); draw=ImageDraw.Draw(canvas)
    for row,name in enumerate(('color','gray')):
        for col,variant in enumerate(('baseline','calibrated','local')):
            im=Image.open(out/(name+'-'+variant+'.png'));canvas.paste(im,(col*640,row*520+40))
            draw.text((col*640+12,row*520+12),name+' / '+variant,fill='black')
    canvas.save(out/'comparison.png',icc_profile=ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes())
    overlay=Image.open(out/'color-calibrated.png');d=ImageDraw.Draw(overlay)
    for i,(x,y) in enumerate(centers): d.ellipse((x-5,y-5,x+5,y+5),outline='white');d.text((x+6,y),str(i+1),fill='white')
    overlay.save(out/'sampling.png')
    (out/'profile.json').write_text(json.dumps(dict(source=SOURCE,working_space='linear sRGB D65',matrix=matrix.tolist(),
        gray_capture_matrix=gray_matrix.tolist(),black=black,lens_shading=shading.tolist(),camera_calibration=False,scope='this screen and capture setup; verify other illuminants before reuse'),indent=2)+'\n')
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report['color_fit'],indent=2))


if __name__ == '__main__': main()
