"""Generate a 21-step neutral sRGB display chart for paired Film3DS gray QA.

python3 tools/gray_chart.py --out Film3DS-gray21.png

Display at 100% pixel size with auto brightness/Night Shift/HDR/dynamic
contrast off. The calibration measures this screen+lighting setup, not a
universal sensor gamut. Do not scale without recording the final chart bounds.
"""
import argparse
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out",type=Path,required=True)
    a=p.parse_args()
    from PIL import Image, ImageDraw
    image=Image.new("RGB",(1680,920),(32,32,32))
    draw=ImageDraw.Draw(image)
    # 21 bars of 80 px, increasing left to right; no labels inside sampling ROI.
    for i in range(21):
        level=round(i*255/20)
        draw.rectangle((i*80,110,(i+1)*80-1,809),fill=(level,level,level))
        draw.text((i*80+8,835),f"{i:02d}",fill=(220,220,220))
    draw.text((20,25),"Film3DS 21-step native AE / RAW gray reference",fill=(240,240,240))
    draw.text((20,885),"Sample only inside the central region of each bar. Keep the display fixed.",fill=(240,240,240))
    if a.out.exists():
        raise SystemExit("Refusing to overwrite existing calibration chart")
    image.save(a.out,format="PNG",icc_profile=None)
    print(a.out)
if __name__=="__main__":main()
