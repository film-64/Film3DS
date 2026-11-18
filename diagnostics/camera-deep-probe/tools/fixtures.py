#!/usr/bin/env python3
"""Generate synthetic CUBE fixtures. No vendor LUTs are required."""
from pathlib import Path
import sys


def generate(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    transforms = {
        "identity": lambda r, g, b: (r, g, b),
        "swap": lambda r, g, b: (b, g, r),
        "gray": lambda r, g, b: (0.25*r + 0.5*g + 0.25*b,) * 3,
        "tone": lambda r, g, b: (0.1 + 0.8*r, 0.1 + 0.8*g, 0.1 + 0.8*b),
        "nonlinear": lambda r, g, b: (r*g, g*b, b*r),
    }
    for kind, transform in transforms.items():
        for size in (17, 33, 65) if kind == "nonlinear" else (17,):
            with (directory / f"{kind}{size}.cube").open("w", encoding="ascii") as file:
                file.write(f'TITLE "Film3DS # {kind}"\nLUT_3D_SIZE {size}\n')
                grid = [i / (size - 1) for i in range(size)]
                for b in grid:
                    for g in grid:
                        for r in grid:
                            file.write(" ".join(f"{v:.9f}" for v in transform(r, g, b)) + "\n")


if __name__ == "__main__":
    generate(sys.argv[1])
