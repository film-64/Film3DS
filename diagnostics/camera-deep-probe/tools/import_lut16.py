#!/usr/bin/env python3
"""Convert one sRGB OpenSourceLuts asset to Film3DS CUBE and F3PR files."""

import argparse
import hashlib
import json
from pathlib import Path
import struct

from profile import encode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    entries = [item for item in manifest["files"] if Path(item["path"]).name == args.source.name]
    if len(entries) != 1 or args.output.suffix != ".cube":
        parser.error("select one manifest asset and a .cube output")
    entry = entries[0]
    data = args.source.read_bytes()
    if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
        parser.error("LUT does not match manifest size and SHA-256")
    magic, size = struct.unpack_from("<4sI", data)
    if (manifest["format"] != {"type": "lut16", "size": size, "input": "sRGB", "output": "sRGB"}
            or magic != b"LUT1" or size not in (17, 33, 65) or len(data) != 8 + size**3 * 6):
        parser.error("invalid or unsupported LUT16 layout")
    source = next(item for item in manifest["sources"] if item["id"] == entry["source"])
    profile_path = args.output.with_suffix(".f3p")
    if args.output.exists() or profile_path.exists():
        parser.error("output or profile already exists")
    with args.output.open("x", encoding="utf-8") as out:
        out.write(f"# {source['attribution']}\n# License: {source['licenseURL']}\n")
        out.write(f"# LUT16 source {entry['path']} SHA-256 {entry['sha256']}\n")
        out.write(f"# Converted from RGB uint16 to CUBE by Film3DS; color samples preserved.\nLUT_3D_SIZE {size}\n")
        for r, g, b in struct.iter_unpack("<HHH", memoryview(data)[8:]):
            out.write(f"{r / 65535:.9f} {g / 65535:.9f} {b / 65535:.9f}\n")
    with profile_path.open("xb") as out:
        out.write(encode({"input": "srgb", "output": "srgb"}))


if __name__ == "__main__":
    main()
