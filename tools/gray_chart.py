"""Generate exact 21-level neutral sRGB reference charts for FilmGrayCal.

Classic (legacy) chart: 1680x920 with dark surrounding space.
Portrait full-frame: 945x1260 (3:4), no labels/margins.
Landscape full-frame: 1260x945 (4:3), same portrait image rotated to
match a native landscape 3DS VGA frame without including monitor background.

  python3 tools/gray_chart.py --out chart.png --format portrait-full
  python3 tools/gray_chart.py --out chart-4x3.png --format landscape-full

Display chart without color management/contrast enhancements whenever possible;
turn off HDR, auto brightness, night shift, and scaling. Actual brightness and
display transfer are not controlled by the image and need empirical measurement.
"""
import argparse
from pathlib import Path


LEVELS = tuple(round(i * 255 / 20) for i in range(21))
PORTRAIT_SIZE = (945, 1260)
LANDSCAPE_SIZE = (1260, 945)


def generate(kind):
    from PIL import Image, ImageDraw
    if kind in ("portrait-full", "landscape-full"):
        # Exactly 21 full-width rows, 60 pixels high each. Pixel RGB is
        # (code,code,code) with no labels, margins, borders or interpolation.
        image = Image.new("RGB", PORTRAIT_SIZE)
        draw = ImageDraw.Draw(image)
        for i, code in enumerate(LEVELS):
            draw.rectangle((0, i * 60, PORTRAIT_SIZE[0] - 1, (i + 1) * 60 - 1),
                           fill=(code, code, code))
        if kind == "landscape-full":
            image = image.transpose(Image.Transpose.ROTATE_90)
            assert image.size == LANDSCAPE_SIZE
        return image
    if kind != "classic":
        raise ValueError("unknown gray chart format")
    image = Image.new("RGB", (1680, 920), (32, 32, 32))
    draw = ImageDraw.Draw(image)
    for i, code in enumerate(LEVELS):
        draw.rectangle((i * 80, 110, (i + 1) * 80 - 1, 809),
                       fill=(code, code, code))
        draw.text((i * 80 + 8, 835), f"{i:02d}", fill=(220, 220, 220))
    draw.text((20, 25), "Film3DS 21-step native AE / RAW gray reference",
              fill=(240, 240, 240))
    draw.text((20, 885), "Sample only inside the central region of each bar. Keep the display fixed.",
              fill=(240, 240, 240))
    return image


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--format", choices=("classic", "portrait-full", "landscape-full"),
                   default="classic")
    args = p.parse_args()
    if args.out.exists():
        raise SystemExit("Refusing to overwrite an existing calibration chart")
    image = generate(args.format)
    image.save(args.out, format="PNG", icc_profile=None)
    print(f"{args.out}: {image.width}x{image.height} {args.format}")


if __name__ == "__main__":
    main()
