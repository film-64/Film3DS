#!/usr/bin/env python3
"""Export a Leitz Phone M9 LUT using OffLineM9's trigger and blend implementation."""

import argparse
from pathlib import Path

from profile import encode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--scene", default="common")
    parser.add_argument("--lux-index", type=int, required=True)
    parser.add_argument("--cct", type=int, required=True)
    args = parser.parse_args()
    if args.output.suffix.lower() != ".cube":
        parser.error("output must end in .cube")
    try:
        from leicafilter import LeicaFilterParams
    except ImportError:
        parser.error("put the OffLineM9 m9 directory on PYTHONPATH and install NumPy")
    source = LeicaFilterParams(str(args.source))
    if args.scene not in source.scenes:
        parser.error("unknown scene in source file")
    lut = source.lut(args.scene, args.lux_index, args.cct)
    # The phone stores [r, g, b] with BGR texels. CUBE is red-fastest with RGB texels.
    samples = lut.transpose(2, 1, 0, 3)[..., ::-1].reshape(-1, 3)
    with args.output.open("x", encoding="ascii") as output:
        output.write("# FILM3DS_TRILINEAR\n")
        output.write(f"# Leitz Phone M9 {source.version}; {args.scene}; "
                     f"lux-index {args.lux_index}; {args.cct} K\n")
        output.write(f"LUT_3D_SIZE {source.lut_n}\n")
        for red, green, blue in samples:
            output.write(f"{red / 255:.9f} {green / 255:.9f} {blue / 255:.9f}\n")
    with args.output.with_suffix(".f3p").open("xb") as profile:
        profile.write(encode({"input": "srgb", "output": "srgb"}))


if __name__ == "__main__":
    main()
