#!/usr/bin/env python3
"""Bind explicit encodings/RAW-knee policy to a CUBE without changing its bytes."""
import argparse
from pathlib import Path
import zlib

ENCODINGS = ("SRGB", "LINEAR_SRGB", "REC709", "FLOG2C")


def create(cube, input_encoding, output_encoding, shoulder, output_intent=None, display_mapping=None):
    cube = Path(cube)
    if input_encoding not in ENCODINGS or output_encoding not in ENCODINGS:
        raise ValueError("unsupported color encoding")
    if shoulder not in ("preserve", "bypass"):
        raise ValueError("unsupported pre-LUT shoulder policy")
    version2 = output_intent is not None or display_mapping is not None
    if version2:
        if (output_intent, display_mapping) not in (("display", "none"), ("scene", "peak-shoulder")):
            raise ValueError("specify display/none or scene/peak-shoulder together")
        if output_intent == "scene" and shoulder != "bypass":
            raise ValueError("scene output mapping requires pre-LUT shoulder bypass")
    size, crc = 0, 0
    with cube.open("rb") as source:
        while chunk := source.read(65536):
            size += len(chunk)
            crc = zlib.crc32(chunk, crc)
    if not size:
        raise ValueError("empty CUBE")
    text = (f"F3LUT{2 if version2 else 1}\ncube_bytes {size}\ncube_crc32 {crc:08x}\n"
            f"input {input_encoding}\noutput {output_encoding}\n"
            f"pre_lut_shoulder {shoulder}\n")
    if version2:
        text += f"output_intent {output_intent}\ndisplay_mapping {display_mapping}\n"
    destination = Path(str(cube) + ".f3a")
    # Exclusive create: never replace user LUTs or an existing adapter.
    with destination.open("x", encoding="ascii", newline="\n") as output:
        output.write(text)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cube", type=Path)
    parser.add_argument("--input", required=True, choices=ENCODINGS)
    parser.add_argument("--output", required=True, choices=ENCODINGS)
    parser.add_argument("--pre-lut-shoulder", required=True, choices=("preserve", "bypass"))
    parser.add_argument("--output-intent", choices=("display", "scene"))
    parser.add_argument("--display-mapping", choices=("none", "peak-shoulder"))
    args = parser.parse_args()
    try:
        print(create(args.cube, args.input, args.output, args.pre_lut_shoulder,
                     args.output_intent, args.display_mapping))
    except (OSError, ValueError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
