#!/usr/bin/env python3
"""Validate an independent capture against a SHA-pinned, fixed luminance model."""
import argparse
import hashlib
import json
import math
from pathlib import Path

from exposure_luma_transfer import image_y, check_pair, np, verify_dense
from exposure_sweep_contract import CONTRACT_MATRIX, EXPOSURE_LUMA_SOURCE, EXPOSURE_LUMA_VALIDATE_SOURCE


def fingerprint(snapshot):
    rule = CONTRACT_MATRIX[EXPOSURE_LUMA_VALIDATE_SOURCE]['pipeline_match']
    return dict(flat={k: snapshot[k] for k in rule['flat']}, matrix_mode=snapshot['matrix_mode'],
                rebuild_timing={k: snapshot['rebuild_timing'][k] for k in rule['rebuild_timing']})


def pipeline_differences(actual, expected):
    differences = {}
    for group in expected:
        for key in expected[group]:
            if actual[group].get(key) != expected[group][key]:
                differences[f'{group}/{key}'] = dict(expected=expected[group][key], actual=actual[group].get(key))
    return differences


def check_observation(images, units, model, offset, expected_shift=0):
    checked = check_pair(images, units, model)
    residuals = [im['exposure_shift_EV'] - offset - expected_shift for im in checked['images']] if checked['in_domain'] else None
    return dict(in_domain=checked['in_domain'], images=checked['images'], residuals_EV=residuals,
                passed=residuals is not None and all(abs(x) <= CONTRACT_MATRIX[EXPOSURE_LUMA_VALIDATE_SOURCE]['max_error_EV'] for x in residuals))


def reference_offset(images, units, model):
    checked = check_pair(images, units, model)
    if not checked['in_domain']:
        raise ValueError('reference start outside fixed model domain')
    values = [x['exposure_shift_EV'] for x in checked['images']]
    if max(values) - min(values) > CONTRACT_MATRIX[EXPOSURE_LUMA_VALIDATE_SOURCE]['max_error_EV']:
        raise ValueError('reference start images disagree')
    return float(np.mean(values))


def load_model(path, expected_sha256):
    raw = Path(path).read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError('fixed model SHA256 mismatch')
    model = json.loads(raw)
    if model['schema'] != EXPOSURE_LUMA_SOURCE or len(model['profiles']) != 2 or {b['fps'] for b in model['profiles']} != {5, 15} or not all(b['relative_luma_transfer_passed'] is True for b in model['profiles']):
        raise ValueError('unregistered or unvalidated fixed model')
    return model


def analyse(directory, model_path, model_sha256, source=EXPOSURE_LUMA_VALIDATE_SOURCE):
    if source != EXPOSURE_LUMA_VALIDATE_SOURCE:
        raise ValueError('unregistered fixed-model validation source')
    fixed = load_model(model_path, model_sha256)
    p = Path(directory)
    verified = verify_dense(p)
    if not verified['passed'] or verified['source'] != CONTRACT_MATRIX[source]['input_source']:
        raise ValueError('unverified or unknown capture source')
    manifest_sha = hashlib.sha256((p / 'manifest.txt').read_bytes()).hexdigest()
    if manifest_sha == fixed['manifest_sha256']:
        raise ValueError('independent capture required; model training batch rejected')
    results = []
    for b in verified['blocks']:
        base = next(x for x in fixed['profiles'] if x['fps'] == b['fps'])
        expected = fingerprint(base['pipeline_fingerprint'])
        mismatches = {when: pipeline_differences(fingerprint(b['snapshots'][when]), expected) for when in ('before_stream', 'after_stop')}
        if b['pipeline_endpoint_changes'] or any(mismatches.values()):
            results.append(dict(fps=b['fps'], passed=False, reason='pipeline_mismatch', pipeline_mismatches=mismatches))
            continue
        if not b['anchor_coverage']:
            results.append(dict(fps=b['fps'], passed=False, reason='gain_anchor_coverage_failure'))
            continue
        def pair(pt):
            return [image_y(p / name) for name in pt['images'][-2:]]
        def units(pt):
            return pt['coarse'] + b['baseline']['3014'] / b['baseline']['300c']
        start = next(x for x in b['points'] if x['kind'] == 'reference_start')
        try:
            offset = reference_offset(pair(start), units(start), base['model'])
        except ValueError as exc:
            results.append(dict(fps=b['fps'], passed=False, reason=str(exc)))
            continue
        checks = []
        for pt in b['points']:
            if pt['kind'] == 'reference_start':
                continue
            expected = 0
            if pt['kind'] == 'gain':
                transfer = next(x for x in base['gain_transfer_checks'] if x['core_code'] == pt['core'] and x['ISP'] == pt['ISP_target'][0])
                rows = [x for x in base['gain_observations'] if x['core_code'] == pt['core'] and x['ISP'] == pt['ISP_target'][0]]
                if transfer['passed'] is not True:
                    checks.append(dict(point=pt['index'], kind=pt['kind'], passed=False, in_domain=False, reason='fixed_gain_transfer_unvalidated'))
                    continue
                expected = float(np.mean([x['effective_luma_shift_EV'] for x in rows]))
            checked = check_observation(pair(pt), units(pt), base['model'], offset, expected)
            checks.append(dict(point=pt['index'], kind=pt['kind'], coarse=pt['coarse'], core_code=pt['core'], ISP=pt['ISP_target'][0], expected_shift_EV=expected, **checked))
        usable_coarse = [x for x in checks if x['kind'] == 'coarse' and x['in_domain']]
        mandatory = [x for x in checks if x['kind'] in ('reference_end', 'gain')]
        passed = len(usable_coarse) >= CONTRACT_MATRIX[source]['minimum_coarse_holdouts'] and all(x['passed'] for x in usable_coarse + mandatory)
        results.append(dict(fps=b['fps'], passed=passed, reason='fixed_model_validation_pass' if passed else 'fixed_model_validation_failure', reference_start_offset_EV=offset, checks=checks))
    return dict(schema=source, contract=CONTRACT_MATRIX[source], frozen_model_sha256=model_sha256, manifest_sha256=manifest_sha,
                analysis_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                profiles=results, passed=len(results) == 2 and all(x['passed'] for x in results), model_refit=False,
                absolute_calibration_validated=False, optical_time_verified=False, external_reference_used_for_correction=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--model-sha256', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = analyse(args.directory, args.model, args.model_sha256)
    with args.out.open('x') as f:
        json.dump(result, f, indent=2)
        f.write('\n')
    print(json.dumps(dict(passed=result['passed'], profiles=[dict(fps=x['fps'], passed=x['passed'], reason=x['reason']) for x in result['profiles']])))
