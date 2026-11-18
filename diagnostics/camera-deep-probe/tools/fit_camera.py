#!/usr/bin/env python3
"""Fit a Film3DS input matrix from paired, unclipped patch centers."""
import argparse
import csv
from pathlib import Path

import numpy as np
from PIL import Image


def decode(code):
    return np.where(code <= .04045, code / 12.92, ((code + .055) / 1.055) ** 2.4)


def encode(linear):
    linear = np.clip(linear, 0, 1)
    return np.where(linear <= .0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - .055)


def patch(image, x, y, radius):
    if not (radius <= x < image.shape[1] - radius and radius <= y < image.shape[0] - radius):
        raise ValueError(f"patch center ({x}, {y}) is too close to the image edge")
    window = image[y-radius:y+radius+1, x-radius:x+radius+1]
    if np.any((window == 0).mean(axis=(0, 1)) > .25) or np.any((window == 255).mean(axis=(0, 1)) > .25):
        raise ValueError(f"patch center ({x}, {y}) contains clipped samples")
    return np.median(window, axis=(0, 1)) / 255


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("camera", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("centers", type=Path, help="CSV with src_x,src_y,ref_x,ref_y,split")
    parser.add_argument("matrix", type=Path)
    parser.add_argument("--radius", type=int, default=4)
    args = parser.parse_args()
    try:
        if args.radius < 1:
            raise ValueError("radius must be positive")
        with Image.open(args.camera) as camera, Image.open(args.reference) as reference:
            src, ref = np.array(camera.convert("RGB")), np.array(reference.convert("RGB"))
        patches = {"fit": [], "check": []}
        with args.centers.open(newline="") as file:
            for row in csv.DictReader(file):
                x, y, u, v = (int(row[key]) for key in ("src_x", "src_y", "ref_x", "ref_y"))
                patches[row["split"]].append((patch(src, x, y, args.radius),
                                               patch(ref, u, v, args.radius)))
        if len(patches["fit"]) < 6 or len(patches["check"]) < 3:
            raise ValueError("provide at least six fit and three independent check patches")
        fit_src, fit_ref = np.array(patches["fit"]).transpose(1, 0, 2)
        matrix, _, rank, singular = np.linalg.lstsq(decode(fit_src), decode(fit_ref), rcond=None)
        if rank < 3 or singular[0] / singular[-1] > 100 or np.max(np.abs(matrix)) > 4:
            raise ValueError("patches do not support a stable 3x3 fit")
        check_src, check_ref = np.array(patches["check"]).transpose(1, 0, 2)
        mapped = encode(decode(check_src) @ matrix)
        before = np.sqrt(np.mean((check_src - check_ref) ** 2)) * 255
        after = np.sqrt(np.mean((mapped - check_ref) ** 2)) * 255
        if after >= before:
            raise ValueError(f"held-out RGB RMS did not improve ({before:.2f} to {after:.2f} codes)")
        with args.matrix.open("x") as file:
            np.savetxt(file, matrix.T, fmt="%.9f")
        print(f"Held-out RGB RMS {before:.2f} to {after:.2f} codes ({len(check_src)} patches)")
    except (OSError, ValueError, KeyError, np.linalg.LinAlgError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
