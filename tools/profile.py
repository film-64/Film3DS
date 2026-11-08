#!/usr/bin/env python3
"""Encode an explicit JSON color recipe as Film3DS F3PR v1."""
import argparse
import json
import math
from pathlib import Path
import struct
import zlib

TRANSFERS = {"srgb": 0, "linear-srgb": 1, "rec709": 3}


def bounded(value, minimum, maximum):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("expected a number")
    if not math.isfinite(value) or not minimum <= value <= maximum:
        raise ValueError(f"expected a value in [{minimum}, {maximum}]")
    return value


def encode(profile):
    if set(profile) - {"input", "output", "strength", "exposure", "global", "zones"}:
        raise ValueError("unknown profile field")
    transfers = [TRANSFERS[profile[key]] for key in ("input", "output")]
    values = [bounded(profile.get("strength", 1), 0, 1),
              bounded(profile.get("exposure", 0), -8, 8)]
    zones = profile.get("zones", [[0, 0, 0]] * 12)
    if len(zones) != 12:
        raise ValueError("expected 12 hue zones")
    for hsl in [profile.get("global", [0, 0, 0]), *zones]:
        hue, saturation, lightness = hsl
        values.extend([bounded(hue, -180, 180), bounded(saturation, -1, 1),
                       bounded(lightness, -1, 1)])
    record = struct.pack("<4sIII41f", b"F3PR", 1, *transfers, *values)
    return record + struct.pack("<I", zlib.crc32(record))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        data = encode(json.loads(args.input.read_text()))
        with args.output.open("xb") as output:
            output.write(data)
    except (KeyError, ValueError, TypeError, OSError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
