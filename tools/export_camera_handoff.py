#!/usr/bin/env python3
"""Publish unchanged text evidence and image omissions from local capture archives."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
from exposure_sweep_contract import CONTRACT_MATRIX, CAMERA_HANDOFF_SOURCE

RESULTS = {'diagnostic-summary.json', 'observation-summary.json', 'recovery-summary.json',
           'external-reference.json', 'reference-before.json', 'reference-after.json',
           'session-status.json', 'frozen-resume.json',
           'fixed-model-validation.json', 'fixed-model-validation-final.json',
           'fit.json', 'analysis.json', 'analysis-final.json'}
PIXELS = {'.pgm', '.ppm', '.yuyv', '.yuv', '.rgb', '.raw', '.rgb565', '.f3s',
          '.png', '.jpg', '.jpeg', '.bmp', '.tif', '.tiff'}


def export(source_root, output, source=CAMERA_HANDOFF_SOURCE):
    if source != CAMERA_HANDOFF_SOURCE or CONTRACT_MATRIX[source]['compute'] is not False:
        raise ValueError('unknown or compute publication boundary')
    source_root, output = Path(source_root).resolve(), Path(output).resolve()
    if output == source_root or source_root in output.parents:
        raise ValueError('publication must not modify the source archive')
    batches = [p for p in sorted(source_root.iterdir()) if p.is_dir() and re.fullmatch(r'batch\d{2}-.+', p.name) and not p.name.endswith('preflight')]
    if not batches:
        raise ValueError('no capture archives')
    output.mkdir(parents=True, exist_ok=False)
    records, omissions = [], []
    def copy(path, target):
        raw = path.read_bytes()
        if b'\0' in raw:
            raise ValueError('binary content in text evidence')
        raw.decode('utf-8')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    for batch in batches:
        dest = output / 'evidence' / batch.name
        reports = []
        for f in sorted(batch.iterdir()):
            if not f.is_file():
                continue
            if f.name in RESULTS or f.name.endswith('-report.json') or f.name in ('FINDINGS.md', 'VALIDATION.md') or f.suffix == '.csv':
                copy(f, dest / f.name)
                if f.suffix == '.json':
                    value = json.loads(f.read_text())
                    if isinstance(value, dict):
                        reports.append(dict(file=f.name, schema=value.get('schema'), source=value.get('source'), passed=value.get('passed'), calibration_validated=value.get('calibration_validated')))
            if f.name in ('batch-sha256.json', 'sha256.json'):
                copy(f, dest / 'original-provenance' / f.name)
        recovered = batch / 'recovered'
        original_count = text_count = 0
        if recovered.is_dir():
            for f in sorted(recovered.iterdir()):
                if not f.is_file():
                    continue
                raw = f.read_bytes()
                if f.suffix in PIXELS:
                    omissions.append(dict(batch=batch.name, path=f.name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(), reason='test_pixels_excluded'))
                    original_count += 1
                elif f.suffix in ('.csv', '.txt', '.json', '.jsonl') and '.partial.' not in f.name and f.name != 'ram-source.json':
                    copy(f, dest / 'recovered' / f.name)
                    text_count += 1
        deployment = batch / 'deployment.json'
        if deployment.is_file():
            value = json.loads(deployment.read_text())
            provenance = {k: value[k] for k in ('source', 'binary_sha256', 'source_sha256', 'arguments', 'session_contract') if k in value}
            provenance['original_deployment_json_sha256'] = hashlib.sha256(deployment.read_bytes()).hexdigest()
            dest.mkdir(parents=True, exist_ok=True)
            (dest / 'original-provenance/deployment-public.json').parent.mkdir(parents=True, exist_ok=True)
            (dest / 'original-provenance/deployment-public.json').write_text(json.dumps(provenance, indent=2) + '\n')
        records.append(dict(batch=batch.name, text_recovery_files=text_count, omitted_pixel_files=original_count, reports=reports))
    (output / 'index.json').write_text(json.dumps(dict(schema=source, as_of='2026-11-14', batches=records,
        images_included=False, capture_reverified=False, calibration_validated=False,
        note='Historical report outcomes copied unchanged; public subset cannot rerun image-complete verifiers.'), indent=2) + '\n')
    (output / 'omitted-images.json').write_text(json.dumps(dict(schema=source, images_included=False, files=omissions), indent=2) + '\n')
    return dict(batches=len(records), omitted_images=len(omissions))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source_root', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    print(json.dumps(export(args.source_root, args.out)))
