#!/usr/bin/env python3
"""Verify an image-free publication subset, not hardware calibration."""
import argparse
import fnmatch
import hashlib
import json
from pathlib import Path, PurePosixPath
from exposure_sweep_contract import CONTRACT_MATRIX, CAMERA_HANDOFF_SOURCE

MANIFEST = 'handoff/camera-20261114/files-sha256.json'


def safe_file(root, name):
    parts = PurePosixPath(name)
    if not name or '\\' in name or parts.is_absolute() or '..' in parts.parts or str(parts) != name:
        raise ValueError('invalid publication path')
    path = root / parts
    if any((root / PurePosixPath(*parts.parts[:i])).is_symlink() for i in range(1, len(parts.parts) + 1)):
        raise ValueError('publication symlink rejected')
    rule = CONTRACT_MATRIX[CAMERA_HANDOFF_SOURCE]
    if path.suffix not in rule['text_suffixes'] and path.name not in rule['text_names']:
        raise ValueError('non-text/image/archive/binary publication rejected')
    if not path.is_file():
        raise ValueError(f'missing publication file: {name}')
    raw = path.read_bytes()
    if b'\0' in raw:
        raise ValueError('binary content rejected')
    raw.decode('utf-8')
    return raw


def verify(root, manifest=MANIFEST):
    root = Path(root).resolve()
    data = json.loads(safe_file(root, manifest))
    if data['schema'] != CAMERA_HANDOFF_SOURCE:
        raise ValueError('unknown publication source')
    if data.get('images_included') is not False or data.get('calibration_validated') is not False:
        raise ValueError('publication cannot claim images or calibration proof')
    names = []
    for item in data['files']:
        name = item['path']
        if name == manifest or name in names:
            raise ValueError('duplicate/self-referential publication entry')
        raw = safe_file(root, name)
        if len(raw) != item['bytes'] or hashlib.sha256(raw).hexdigest() != item['sha256']:
            raise ValueError(f'publication integrity mismatch: {name}')
        names.append(name)
    if not names:
        raise ValueError('empty publication')
    listed = set(names) | {manifest}
    for relative in CONTRACT_MATRIX[CAMERA_HANDOFF_SOURCE]['scope_roots']:
        prefix = root / relative
        for path in prefix.rglob('*'):
            if path.is_symlink():
                raise ValueError('publication symlink rejected')
            if not path.is_file():
                continue
            # Local compilation/bytecode is not publication content.
            exclusions = CONTRACT_MATRIX[CAMERA_HANDOFF_SOURCE]['local_generated_exclusions']
            if relative.startswith('diagnostics/') and any(fnmatch.fnmatchcase(part, pattern) for part in path.relative_to(prefix).parts for pattern in exclusions):
                continue
            if str(path.relative_to(root)) not in listed:
                raise ValueError(f'unlisted publication file: {path.relative_to(root)}')
    return dict(schema=CAMERA_HANDOFF_SOURCE, package_verified=True, files=len(names),
                bytes=sum(x['bytes'] for x in data['files']), images_included=False,
                calibration_validated=False, capture_reverified=False)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--manifest', default=MANIFEST)
    args = p.parse_args()
    print(json.dumps(verify(args.root, args.manifest)))
