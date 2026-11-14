#!/usr/bin/env python3
"""Measure grain in renders of Film3DS's nine-patch gray chart."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageCms

LEVELS = (24, 48, 72, 104, 128, 156, 184, 208, 232)
SIDE = 1536
PATCH = SIDE // 3


def write_chart(path):
    image = Image.new("RGB", (SIDE, SIDE))
    for index, code in enumerate(LEVELS):
        x, y = index % 3 * PATCH, index // 3 * PATCH
        image.paste((code, code, code), (x, y, x + PATCH, y + PATCH))
    icc = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB"))
    with path.open("xb") as file:
        image.save(file, format="TIFF", icc_profile=icc.tobytes())


def measure(path):
    with Image.open(path) as image:
        if image.size != (SIDE, SIDE):
            raise ValueError(f"{path}: expected the original {SIDE}x{SIDE} chart")
        # Pillow preserves the 16-bit single-channel reference PNGs.
        if image.mode in ("I", "I;16", "I;16B", "I;16L"):
            y = np.asarray(image, dtype=np.float64) / 257
        else:
            rgb = np.asarray(image.convert("RGB"), dtype=np.float64)
            y = rgb @ [.2126, .7152, .0722]
    rows = []
    margin = PATCH // 4
    for i, code in enumerate(LEVELS):
        x, yy = i % 3 * PATCH, i // 3 * PATCH
        patch = y[yy + margin:yy + PATCH - margin, x + margin:x + PATCH - margin]
        mean = patch.mean()
        residual = patch - mean
        sigma = residual.std()
        correlation = []
        for lag in (1, 2, 4):
            product_x = (residual[:, :-lag] * residual[:, lag:]).mean()
            product_y = (residual[:-lag] * residual[lag:]).mean()
            correlation.append(float((product_x + product_y) / (2 * sigma**2)) if sigma else None)
        rows.append({
            "chart_code": code,
            "rendered_mean_code": float(mean),
            "grain_rms_code": float(sigma),
            "absolute_grain_p95_code": float(np.percentile(np.abs(residual), 95)),
            "correlation_at_1_2_4_px": correlation,
        })
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renders", nargs="*", type=Path)
    parser.add_argument("--chart", type=Path, help="generate the input chart as a TIFF")
    parser.add_argument("--out", type=Path, help="write measured statistics as JSON")
    parser.add_argument("--profile", type=Path, help="also write a Film3DS F3G1 fit for a single render")
    args = parser.parse_args()
    try:
        if args.chart and not (args.renders or args.out or args.profile):
            write_chart(args.chart)
            return
        if args.chart or not args.renders or not args.out:
            raise ValueError("use --chart alone, or provide rendered files and --out")
        if args.profile and len(args.renders) != 1:
            raise ValueError("--profile requires one render")
        stats = {path.name: measure(path) for path in args.renders}
        with args.out.open("x") as output:
            json.dump(stats, output, indent=2)
            output.write("\n")
        if args.profile:
            rows = next(iter(stats.values()))
            if not all(a["rendered_mean_code"] < b["rendered_mean_code"] for a, b in zip(rows, rows[1:])):
                raise ValueError("clipped chart bands overlap; adjust the render before fitting")
            if any(row["grain_rms_code"] > 32 for row in rows):
                raise ValueError("grain RMS exceeds the F3G1 range")
            with args.profile.open("x") as output:
                output.write("F3G1\n")
                for row in rows:
                    corr = row["correlation_at_1_2_4_px"][0] or 0
                    corr = min(.95, max(-.95, corr))
                    output.write(f'{row["rendered_mean_code"]:.6f} {row["grain_rms_code"]:.6f} {corr:.6f}\n')
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
