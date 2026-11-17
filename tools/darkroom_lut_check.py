"""Compare packaged film LUTs through the real offline C developer, without a camera.

Creates a synthetic sRGB target (not a measured ColorChecker), F3S source, two
developed JPEGs and a comparison figure. Existing output directories are refused.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

from PIL import Image, ImageChops, ImageDraw, ImageStat

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--luts", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/filmctl")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    stocks = ("00A Portra 400 Endura", "00B Gold 200 Endura")
    colors = ((192, 128, 96), (224, 176, 144), (128, 80, 56), (240, 208, 176),
              (192, 48, 32), (208, 160, 32), (40, 136, 56), (32, 80, 184),
              (32, 160, 176), (176, 48, 144), (96, 64, 144), (184, 112, 48),
              (32, 32, 32), (80, 80, 80), (160, 160, 160), (224, 224, 224))
    target = Image.new("RGB", (640, 480))
    draw = ImageDraw.Draw(target)
    for i, color in enumerate(colors):
        x, y = i % 4 * 160, i // 4 * 96
        draw.rectangle((x, y, x + 159, y + 95), fill=color)
    for x in range(640):
        code = round(x * 255 / 639)
        draw.line((x, 384, x, 479), fill=(code, code, code))
    target.save(args.out / "input.png")
    # F3SR v1: neutral recipe, full sRGB RGB24 source, CRC over header and pixels.
    header = bytearray(304)
    struct.pack_into("<4s6I", header, 0, b"F3SR", 1, 304, 640 * 480 * 3, 640, 480, 3)
    struct.pack_into("<I", header, 60, 100)
    struct.pack_into("<II41f", header, 128, 0, 0, 1, 0, *([0] * 39))
    pixels = target.tobytes()
    struct.pack_into("<I", header, 300, zlib.crc32(header[:300] + pixels))
    source = args.out / "input.f3s"
    source.write_bytes(header + pixels)
    images = [target]
    records = []
    for stock in stocks:
        cube = args.luts / (stock + ".cube")
        profile = cube.with_suffix(".f3p")
        output = args.out / (stock + ".jpg")
        subprocess.run([str(args.binary.resolve()), "develop", str(source), str(output),
                        str(cube), str(profile)], check=True)
        with Image.open(output) as decoded:
            images.append(decoded.convert("RGB"))
        records.append(dict(name=stock, cube_sha256=hashlib.sha256(cube.read_bytes()).hexdigest(),
                            profile_sha256=hashlib.sha256(profile.read_bytes()).hexdigest()))
    difference = ImageChops.difference(images[1], images[2])
    stats = ImageStat.Stat(difference)
    report = dict(target="synthetic sRGB patches and gray ramp; not measured film calibration",
                  pipeline="build/filmctl develop; same F3S source; full LUT strength; no grain or sharpening",
                  assets=records, identical_pixels=images[1].tobytes() == images[2].tobytes(),
                  mean_abs_8bit_difference_per_channel=stats.mean,
                  max_abs_8bit_difference_per_channel=[v[1] for v in difference.getextrema()])
    if report["identical_pixels"]:
        raise AssertionError("Portra and Gold developed into identical pixels")
    canvas = Image.new("RGB", (960, 264), (24, 24, 24))
    labels = ("Synthetic sRGB input", "Portra 400 / Endura", "Gold 200 / Endura")
    for i, (image, label) in enumerate(zip(images, labels)):
        canvas.paste(image.resize((320, 240), Image.Resampling.LANCZOS), (i * 320, 24))
        ImageDraw.Draw(canvas).text((i * 320 + 8, 6), label, fill=(240, 240, 240))
    canvas.save(args.out / "comparison.png")
    (args.out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
