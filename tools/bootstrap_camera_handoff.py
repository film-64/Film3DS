#!/usr/bin/env python3
"""Create registered local compatibility links; never contact a camera."""
import argparse
import os
from pathlib import Path
from exposure_sweep_contract import CONTRACT_MATRIX, CAMERA_HANDOFF_BOOTSTRAP_SOURCE


def bootstrap(root, source=CAMERA_HANDOFF_BOOTSTRAP_SOURCE):
    if source != CAMERA_HANDOFF_BOOTSTRAP_SOURCE:
        raise ValueError('unknown bootstrap source')
    root = Path(root).resolve()
    pairs = [(root / a, root / b) for a, b in CONTRACT_MATRIX[source]['links'].items()]
    for link, target in pairs:
        if not target.is_dir():
            raise ValueError(f'missing diagnostic source: {target}')
        if (link.exists() or link.is_symlink()) and link.resolve() != target.resolve():
            raise ValueError(f'existing path preserved: {link}')
    for link, target in pairs:
        if link.is_symlink():
            continue
        if link.exists():
            raise ValueError(f'existing path preserved: {link}')
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(os.path.relpath(target, link.parent), target_is_directory=True)
        print(f'{link.relative_to(root)} -> {target.relative_to(root)}')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    bootstrap(p.parse_args().root)
