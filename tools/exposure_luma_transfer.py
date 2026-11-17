#!/usr/bin/env python3
"""Bounded, fixed-scene center-ROI luminance transfer; never an ISO calibration."""
import argparse
import hashlib
import json
import math
from pathlib import Path

from exposure_response_fit import np
from exposure_dense_report import analyse as verify_dense
from exposure_sweep_contract import CONTRACT_MATRIX, EXPOSURE_LUMA_SOURCE, DENSE_COARSE


def image_y(path):
    raw = Path(path).read_bytes()
    if len(raw) != 24576:
        raise ValueError('invalid center ROI size')
    return np.frombuffer(raw, np.uint8).reshape(96, 128, 2)[..., 0]


def fit_profile(images, times, anchors, source=EXPOSURE_LUMA_SOURCE):
    if source != EXPOSURE_LUMA_SOURCE:
        raise ValueError('unregistered luma analysis source')
    rule = CONTRACT_MATRIX[source]
    n = len(images)
    if n not in (15, 19) or len(times) != n or any(not math.isfinite(t) or t <= 0 for t in times) or any(b <= a for a, b in zip(times, times[1:])):
        raise ValueError('invalid exposure grid')
    if len(anchors) != 2 or len({a['index'] for a in anchors}) != 2 or any(not 0 <= a['index'] < n for a in anchors):
        raise ValueError('two distinct baseline anchor conditions required')
    if abs(math.log2(times[anchors[0]['index']] / times[anchors[1]['index']])) < .5:
        raise ValueError('baseline anchor separation below contract')
    all_pairs = images + [a['images'] for a in anchors]
    if any(len(pair) != 2 for pair in all_pairs):
        raise ValueError('two matching uint8 luminance images required per condition')
    shape = images[0][0].shape
    if len(shape) != 2 or any(len(pair) != 2 or any(im.dtype != np.uint8 or im.shape != shape for im in pair) for pair in all_pairs):
        raise ValueError('two matching uint8 luminance images required per condition')
    excluded = sorted(a['index'] for a in anchors)
    training = [i for i in range(0, n, 2) if i not in excluded]
    holdout = list(range(1, n, 2))
    lo, hi = rule['encoded_Y_domain']
    means = [float(pair[-1].mean()) for pair in images]
    valid = {i for i in training if lo <= means[i] <= hi and float(((images[i][-1] >= lo) & (images[i][-1] <= hi)).mean()) >= rule['holdout_pixel_coverage']}
    plateau = []
    for a, b in zip(training, training[1:]):
        low = float(np.median(images[a][-1]))
        slope = (float(np.median(images[b][-1])) - low) / math.log2(times[b] / times[a])
        if low >= rule['training_tail_plateau']['lower_median_at_least'] and slope < rule['training_tail_plateau']['maximum_code_rise_per_EV']:
            valid.discard(a)
            valid.discard(b)
            plateau.append(dict(lower_index=a, upper_index=b, code_rise_per_EV=slope))
    effective = [i for i in training if i in valid]
    result = dict(relative_luma_transfer_passed=False, calibration_validated=False,
                  optical_time_verified=False, training_indices=training,
                  excluded_anchor_conditions=excluded, holdout_indices=holdout,
                  effective_training_indices=effective, training_plateau_masks=plateau)
    if len(effective) < rule['minimum_training_exposures']:
        return dict(**result, reason='insufficient_training_exposures')
    knots = [means[i] for i in effective]
    if any(b <= a for a, b in zip(knots, knots[1:])):
        return dict(**result, reason='nonmonotonic_training_luminance')
    if knots[-1] - knots[0] < rule['minimum_domain_span']:
        return dict(**result, reason='insufficient_training_code_span')
    model = dict(mean_Y=knots, log2_effective_lines=[math.log2(times[i]) for i in effective])
    checks = [dict(index=i, **check_pair(images[i], times[i], model)) for i in holdout]
    late = [dict(index=a['index'], **check_pair(a['images'], times[a['index']], model)) for a in anchors]
    usable = [x for x in checks if x['in_domain']]
    passed = len(usable) >= rule['minimum_holdout_exposures'] and all(x['passed'] for x in usable) and all(x['passed'] for x in late)
    return dict(**{k: v for k, v in result.items() if k != 'relative_luma_transfer_passed'},
                relative_luma_transfer_passed=passed, reason='holdout_pass' if passed else 'insufficient_or_failed_holdout',
                model=model, domain_mean_Y=[knots[0], knots[-1]], holdout=checks, late_baseline_holdout=late)


def check_pair(images, effective_lines, model):
    rule = CONTRACT_MATRIX[EXPOSURE_LUMA_SOURCE]
    knots = model['mean_Y']
    checks = []
    for im in images:
        mean = float(im.mean())
        coverage = float(((im >= knots[0]) & (im <= knots[-1])).mean())
        valid = knots[0] <= mean <= knots[-1] and coverage >= rule['holdout_pixel_coverage']
        error = float(np.interp(mean, knots, model['log2_effective_lines']) - math.log2(effective_lines)) if valid else None
        checks.append(dict(mean_Y=mean, common_pixel_fraction=coverage, in_domain=valid, exposure_shift_EV=error))
    valid = all(x['in_domain'] for x in checks)
    return dict(in_domain=valid, images=checks,
                passed=valid and all(abs(x['exposure_shift_EV']) <= rule['max_holdout_error_EV'] for x in checks))


def analyse(directory, source=EXPOSURE_LUMA_SOURCE):
    if source != EXPOSURE_LUMA_SOURCE:
        raise ValueError('unregistered luma analysis source')
    p = Path(directory)
    verified = verify_dense(p)
    if not verified['passed'] or verified['source'] != CONTRACT_MATRIX[source]['input_source']:
        raise ValueError('unverified or unknown capture source')
    profiles = []
    for b in verified['blocks']:
        pts = [x for x in b['points'] if x['kind'] == 'coarse']
        if [x['coarse'] for x in pts] != DENSE_COARSE[b['fps']]:
            raise ValueError('unregistered coarse plan')
        if b['pipeline_endpoint_changes'] or not b['anchor_coverage']:
            profiles.append(dict(fps=b['fps'], relative_luma_transfer_passed=False, reason='pipeline_change_or_anchor_coverage_failure'))
            continue
        def pair(pt):
            return [image_y(p / name) for name in pt['images'][-2:]]
        baseline = b['baseline']
        def units(pt):
            return pt['coarse'] + baseline['3014'] / baseline['300c']
        late = [x for x in b['points'] if x['kind'] == 'gain' and x['core'] == 528 and x['ISP_target'] == [128] * 4]
        anchors = [dict(index=next(i for i, pt in enumerate(pts) if pt['coarse'] == x['coarse']), images=pair(x)) for x in late]
        fit = fit_profile([pair(x) for x in pts], [units(x) for x in pts], anchors)
        fit.update(fps=b['fps'], pipeline_endpoint_changes=b['pipeline_endpoint_changes'],
                   pipeline_fingerprint=b['snapshots']['before_stream'], Y2R_matrix=verified['Y2R_matrix'],
                   timing_baseline=baseline, optical_seconds_per_line_estimate=pts[0]['estimated_seconds'] / units(pts[0]) if pts[0]['estimated_seconds'] else None)
        gains = []
        if fit['relative_luma_transfer_passed']:
            for pt in b['points']:
                if pt['kind'] != 'gain':
                    continue
                check = check_pair(pair(pt), units(pt), fit['model'])
                shift = float(np.mean([x['exposure_shift_EV'] for x in check['images']])) if check['in_domain'] else None
                gains.append(dict(point=pt['index'], coarse=pt['coarse'], core_code=pt['core'], ISP=pt['ISP_target'][0],
                                  effective_luma_shift_EV=shift, image_checks=check['images']))
        transfers = gain_transfer_checks(gains)
        fit.update(gain_observations=gains, gain_transfer_checks=transfers)
        profiles.append(fit)
    return dict(schema=source, input_source=verified['source'], contract=CONTRACT_MATRIX[source], profiles=profiles,
                analysis_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                manifest_sha256=hashlib.sha256((p / 'manifest.txt').read_bytes()).hexdigest(),
                absolute_calibration_validated=False, optical_time_verified=False, external_reference_used_in_fit=False,
                scope='same_scene_center_ROI_mean_luma_transfer_only_not_general_radiometry_or_RGB_color_calibration')


def gain_transfer_checks(rows):
    result = []
    for core in (528, 544, 1040):
        for isp in (96, 128, 160):
            selected = [x for x in rows if x['core_code'] == core and x['ISP'] == isp]
            valid = len(selected) == 2 and len({x['coarse'] for x in selected}) == 2 and all(x['effective_luma_shift_EV'] is not None for x in selected)
            difference = selected[1]['effective_luma_shift_EV'] - selected[0]['effective_luma_shift_EV'] if valid else None
            individual = [im['exposure_shift_EV'] for row in selected for im in row['image_checks']] if valid else []
            spread = max(individual) - min(individual) if len(individual) == 4 and all(v is not None for v in individual) else None
            result.append(dict(core_code=core, ISP=isp, two_valid_anchors=valid, shift_difference_EV=difference,
                               four_image_shift_range_EV=spread,
                               passed=valid and spread is not None and spread <= CONTRACT_MATRIX[EXPOSURE_LUMA_SOURCE]['max_holdout_error_EV']))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = analyse(args.directory)
    with args.out.open('x') as f:
        json.dump(result, f, indent=2)
        f.write('\n')
    print(json.dumps([dict(fps=x['fps'], passed=x['relative_luma_transfer_passed'], reason=x['reason'],
                           domain=x.get('domain_mean_Y'), holdout=x.get('holdout'), late=x.get('late_baseline_holdout')) for x in result['profiles']]))
