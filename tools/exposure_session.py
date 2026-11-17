#!/usr/bin/env python3
"""Registered hardware exposure-auto/v1 session: deploy once, recover, verify.

No capture retry, device-setting queue or automatic redeployment. Device executes
the bounded feedback/exploration contract. Host polling is read-only readiness.
"""
import argparse
import csv
import hashlib
import http.client
import io
import json
import math
from pathlib import Path
import re
import shutil
import sys
import time
import zlib

import numpy as np
from PIL import Image
from decode_exposure_state import decode, write_report

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "build/worktrees/camera-deep-probe"
sys.path.insert(0, str(PROBE / "tools"))
from run_deep_probe import deploy
from retrieve_probe_ram import get, parse_index, retrieve

ARGS = ("--auto", "--calibrate", "--offline", "--sensor=1", "--ram", "--raw", "--calibration-part=exposure-auto")


def linear(rgb):
    rgb = np.clip(rgb, 0, 1)
    return np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)


def meter_rgb(rgb):
    roi = rgb[192:288, 256:384]
    luminance = linear(roi) @ np.array([.2126, .7152, .0722])
    return {"linear_mean": float(luminance.mean()),
            "linear_p50": float(np.median(luminance)),
            "highlight_fraction": float(np.mean(np.max(roi, axis=2) >= 250 / 255)),
            "black_fraction": float(np.mean(np.max(roi, axis=2) <= 1 / 255))}


def yuyv_rgb(path, matrix):
    content = path.read_bytes()
    if len(content) != 640 * 480 * 2 or matrix not in (0, 1, 2, 3):
        raise ValueError("invalid YUYV size or unregistered color matrix")
    q = np.frombuffer(content, dtype=np.uint8).reshape(480, 320, 4).astype(np.float64)
    yy = np.empty((480, 640))
    yy[:, 0::2], yy[:, 1::2] = q[:, :, 0], q[:, :, 2]
    limited, hd = matrix >= 2, matrix % 2 == 1
    yy = (yy - (16 if limited else 0)) / (219 if limited else 255)
    u = np.repeat((q[:, :, 1] - 128) / (224 if limited else 255), 2, axis=1)
    v = np.repeat((q[:, :, 3] - 128) / (224 if limited else 255), 2, axis=1)
    return np.clip(np.stack([yy + (1.5748 if hd else 1.402) * v,
                            yy - (.187324 if hd else .344136) * u - (.468124 if hd else .714136) * v,
                            yy + (1.8556 if hd else 1.772) * u], axis=-1), 0, 1)


def manifest_records(directory, name):
    rows = []
    for line in (directory / "manifest.txt").read_text().splitlines():
        if line.startswith(name + " "):
            parts = line.split()[1:]
            if any("=" not in p for p in parts):
                raise ValueError("malformed automation manifest")
            row = dict(p.split("=", 1) for p in parts)
            if len(row) != len(parts):
                raise ValueError("duplicate automation field")
            rows.append(row)
    return rows


def verify_registered_plan(stages, meters, version, upper=528,allow_partial=False):
    labels = [s["label"] for s in stages]
    normal = []
    while 2 + len(normal) < len(labels) and labels[2 + len(normal)] == f"auto_normal_{len(normal)}":
        normal.append(labels[2 + len(normal)])
    if len(normal) > 3:
        raise ValueError("normal decision budget exceeded")
    expected = ["auto_seed", "auto_normal_base"] + normal
    expected += ["auto_response_short", "auto_reference_mid", "auto_gain_base"] if version >= 2 else ["auto_gain_base"]
    expected += [f"auto_gain_{code:04x}" for code in (0x210, 0x218, 0x220, 0x228, 0x230, 0x23c)]
    expected += ["auto_digital_2", "auto_digital_4"]
    expected += ["auto_reference_end", "auto_restore"] if version >= 2 else ["auto_restore"]
    if labels != (expected[:len(labels)] if allow_partial else expected) or [s["phase"] for s in stages] != list(range(3, 3 + len(stages))):
        raise ValueError("unregistered stage sequence or omitted phase")
    if allow_partial:return
    lookup = {s["label"]: s for s in stages}
    base = lookup["auto_normal_base"]
    normal_gain = [v["raw"] for v in base["composite_gain_raw"].values()]
    current = base["timing_raw"]["3012"]
    visited = {current}
    for name in normal:
        previous = base if name == normal[0] else lookup[normal[normal.index(name) - 1]]
        luma = float(meters[previous["phase"]]["linear_luma"])
        error = math.log2(.18 / max(luma, 1e-4))
        candidate = min(upper, max(1, math.floor(current * 2 ** (max(-2, min(2, error)) * .5) + .5)))
        stage = lookup[name]
        if abs(error) <= .125 or candidate in visited or stage["timing_raw"]["3012"] != candidate:
            raise ValueError("normal decision violates feedback/unique-point contract")
        if [v["raw"] for v in stage["composite_gain_raw"].values()] != normal_gain:
            raise ValueError("normal exposure changed analogue/color gain")
        visited.add(candidate)
        current = candidate
    for stage in stages[1:-1]:
        if not 1 <= stage["timing_raw"]["3012"] <= upper:
            raise ValueError("diagnostic control exceeds registered interval")
    canonical_high=current
    if version>=4:
        last_normal=lookup[normal[-1]] if normal else base
        analog=last_normal["analog_channel_codes"]["Gr"]["raw"]
        canonical_high=min(upper,max(1,math.floor(current*analog/8+.5)))
    for code in (0x210, 0x218, 0x220, 0x228, 0x230, 0x23c):
        stage = lookup[f"auto_gain_{code:04x}"]
        expected_c = (canonical_high if code<=0x220 else max(1,canonical_high//2)) if version>=5 else (canonical_high if version >= 2 and code == 0x220 else max(1, canonical_high // (2 if version >= 2 else 4)))
        if stage["timing_raw"]["3012"] != expected_c or any(v["raw"] != code for v in stage["composite_gain_raw"].values()):
            raise ValueError("gain exploration differs from registered point")
    for factor in (2, 4):
        stage = lookup[f"auto_digital_{factor}"]
        if stage["timing_raw"]["3012"] != max(1, canonical_high // 4) or any(v["raw"] != ((factor << 9) | 0x20) for v in stage["composite_gain_raw"].values()):
            raise ValueError("digital exploration differs from registered point")


def fit_reference_model(directory, stages, meters, reference, batch):
    """Same-run response identification plus explicit external-reference anchor.

    A post-run meter alone is provisional under natural light. It never becomes
    a certified ISO measurement or an assumed constant-light bracket.
    """
    controls = {s["label"]: s for s in stages if s["proof_status"] == "verified_locked_sample"}
    training = []
    for label, stage in controls.items():
        if label == "auto_gain_base" or label == "auto_normal_base" or re.fullmatch(r"auto_normal_\d+", label):
            if any(v["raw"] != 0x220 for v in stage["composite_gain_raw"].values()):
                raise ValueError("response training changed reference gain")
            training.append((float(meters[stage["phase"]]["linear_luma"]), stage["timing_raw"]["3012"], label))
    training.sort()
    if len(training) < 4 or any(b[0] <= a[0] or b[1] <= a[1] for a, b in zip(training, training[1:])):
        return {"status": "insufficient_monotone_same_run_response", "absolute_iso_calibrated": False}
    coefficient = np.polynomial.polynomial.polyfit([p[0] for p in training], [p[1] for p in training], 2)
    inverse = lambda luma: float(np.polynomial.polynomial.polyval(luma, coefficient))
    checks = []
    for i in range(1, len(training) - 1):
        remaining = training[:i] + training[i + 1:]
        q = np.polynomial.polynomial.polyfit([p[0] for p in remaining], [p[1] for p in remaining], 2)
        predicted = float(np.polynomial.polynomial.polyval(training[i][0], q))
        checks.append({"label": training[i][2], "error_ev": math.log2(predicted / training[i][1])})
    with (directory / "frame_burst.csv").open() as file:
        burst = list(csv.DictReader(file))
    if len(burst) != 24 or any(r["rate"] != "0" or r["wait_result"] != "00000000" or r["received"] != "1" or r["buffer_error"] != "0" for r in burst):
        raise ValueError("missing complete same-run cadence")
    times = [int(r["time_ms"]) for r in sorted(burst, key=lambda r: int(r["frame"]))]
    period = sum(b - a for a, b in zip(times[2:], times[3:])) / 21 / 1000
    frame_lines = controls[training[0][2]]["timing_raw"]["300a"]
    row_seconds = period / frame_lines
    anchor = None
    target_coarse = inverse(.18)
    bound = bool(reference and batch and reference.get("illumination_batch") == batch)
    if bound:
        iso, fnumber, shutter = (float(reference[k]) for k in ("iso", "f_number", "shutter_seconds"))
        if not all(math.isfinite(x) and x > 0 for x in (iso, fnumber, shutter)):
            raise ValueError("invalid external exposure reference")
        camera_f = float(reference.get("nominal_camera_f_number", 2.8))
        anchor = iso * shutter / (target_coarse * row_seconds) * (camera_f / fnumber) ** 2
    gain_rows = []
    for stage in stages:
        if stage["kind"] not in ("code", "digital") or stage["proof_status"] != "verified_locked_sample":
            continue
        luma = float(meters[stage["phase"]]["linear_luma"])
        inside = training[0][0] <= luma <= training[-1][0]
        effective = inverse(luma) / stage["timing_raw"]["3012"] if inside else None
        analog = stage["analog_channel_codes"]["Gr"]["raw"]
        digital = stage["core_digital"]["Gr"]["configured_factor"]
        expected = analog / 8 * digital if analog is not None else None
        gain_rows.append({"label": stage["label"], "coarse": stage["timing_raw"]["3012"],
                          "composite_Gr": stage["composite_gain_raw"]["Gr"]["raw"],
                          "analog_Gr_code": analog, "core_digital_Gr": digital,
                          "effective_gain_from_same_run_response": effective,
                          "candidate_core_code_ratio_times_digital": expected,
                          "gain_model_error_ev": math.log2(effective / expected) if inside and expected else None,
                          "reference_ei": anchor * effective if anchor is not None and effective is not None else None,
                          "status": "within_measured_response_domain" if inside else "needs_brighter_reference_probe_no_extrapolation"})
    gain_errors = [abs(r["gain_model_error_ev"]) for r in gain_rows if r["gain_model_error_ev"] is not None]
    return {"status": "same_run_model_with_provisional_reference" if anchor else "same_run_model_awaiting_reference",
            "equation": "Ceq=a0+a1*L+a2*L^2; L=measured nominal linear center RGB luminance",
            "coefficients": coefficient.tolist(), "training_luma_coarse_label": training,
            "leave_one_exposure_out": checks, "response_max_holdout_ev": max(abs(p["error_ev"]) for p in checks),
            "frame_period_seconds": period, "row_seconds_model": row_seconds,
            "target_coarse_model": target_coarse, "reference_gain_ei": anchor,
            "target_slightly_above_measured_luma": .18 > training[-1][0],
            "gain_rows": gain_rows, "gain_model_max_error_ev": max(gain_errors) if gain_errors else None,
            "absolute_iso_calibrated": False,
            "natural_light_reference": "post-run reference only; repeated fixed-setting anchors and before/after meter readings required for drift-controlled calibration",
            "timing_note": "coarse/cadence model; optical timing not independently measured"}


def fit_natural_light_model(directory, stages, meters, reference, batch):
    """v2 keeps native-WB normal work separate from canonical gain metrology."""
    by_label = {s["label"]: s for s in stages if s["proof_status"] == "verified_locked_sample"}
    refs = manifest_records(directory, "AUTO_REFERENCE")
    if len(refs) != 3 or {r["role"] for r in refs} != {"start", "mid", "end"}:
        raise ValueError("missing/duplicate natural-light anchors")
    by_phase = {s["phase"]: s for s in stages}
    signatures = []
    anchor_rows = []
    for r in refs:
        s = by_phase[int(r["phase"])]
        signatures.append((tuple(s["timing_raw"].values()),
                           tuple(v["raw"] for v in s["composite_gain_raw"].values()),
                           tuple(v["raw"] for v in s["isp_digital"].values())))
        anchor_rows.append({"role": r["role"], "phase": s["phase"], "linear_luma": float(meters[s["phase"]]["linear_luma"])})
    if len(set(signatures)) != 1:
        raise ValueError("light-reference controls changed")
    first_luma = next(r["linear_luma"] for r in anchor_rows if r["role"] == "start")
    if first_luma <= .005:
        return {"status": "rejected_dark_light_anchor", "absolute_iso_calibrated": False}
    for r in anchor_rows:
        r["drift_ev"] = math.log2(max(r["linear_luma"], 1e-12) / first_luma)
    light_drift = max(abs(r["drift_ev"]) for r in anchor_rows)
    names = ("auto_response_short", "auto_gain_base", "auto_gain_0220")
    training = []
    for name in names:
        s = by_label[name]
        if any(v["raw"] != 0x220 for v in s["composite_gain_raw"].values()):
            raise ValueError("canonical response reference gain mismatch")
        training.append((float(meters[s["phase"]]["linear_luma"]), s["timing_raw"]["3012"], name))
    training.sort()
    if any(b[0] <= a[0] or b[1] <= a[1] for a, b in zip(training, training[1:])):
        return {"status": "rejected_nonmonotone_response", "natural_light_anchors": anchor_rows, "absolute_iso_calibrated": False}
    q = np.polynomial.polynomial.polyfit([p[0] for p in training], [p[1] for p in training], 2)
    inverse = lambda x: float(np.polynomial.polynomial.polyval(x, q))
    derivative = np.polynomial.polynomial.polyval(np.linspace(training[0][0], max(.18, training[-1][0]), 100), np.polynomial.polynomial.polyder(q))
    if min(derivative) <= 0:
        return {"status": "rejected_nonmonotone_fit", "absolute_iso_calibrated": False}
    with (directory / "frame_burst.csv").open() as file:
        burst = list(csv.DictReader(file))
    if len(burst) != 24 or any(r["wait_result"] != "00000000" or r["receive_result"] != "00000000" or r["received"] != "1" or r["buffer_error"] != "0" for r in burst):
        raise ValueError("missing cadence proof")
    times = [int(r["time_ms"]) for r in sorted(burst, key=lambda r: int(r["frame"]))]
    period = sum(b - a for a, b in zip(times[2:], times[3:])) / 21 / 1000
    row_seconds = period / by_label["auto_gain_base"]["timing_raw"]["300a"]
    if period <= 0:
        raise ValueError("invalid frame time")
    dose = None
    phone_drift = None
    pair = False
    if reference and reference.get("illumination_batch") == batch:
        readings = ([reference["before"], reference["after"]] if "before" in reference and "after" in reference
                    else [reference["before"]] if "before" in reference else [reference])
        doses = []
        for reading in readings:
            iso, n, t = (float(reading[k]) for k in ("iso", "f_number", "shutter_seconds"))
            if not all(math.isfinite(v) and v > 0 for v in (iso, n, t)):
                raise ValueError("invalid phone reference")
            doses.append(iso * t * (2.8 / n) ** 2)
        dose = math.exp(sum(math.log(x) for x in doses) / len(doses))
        pair = len(doses) == 2
        phone_drift = abs(math.log2(doses[-1] / doses[0])) if pair else None
    target = inverse(.18)
    canonical_ei = dose / (target * row_seconds) if dose else None
    gains = []
    for s in stages:
        if s["kind"] not in ("auto_gain", "auto_digital"):
            continue
        luma = float(meters[s["phase"]]["linear_luma"])
        inside = training[0][0] <= luma <= training[-1][0]
        effective = inverse(luma) / s["timing_raw"]["3012"] if inside else None
        expected = s["analog_channel_codes"]["Gr"]["raw"] / 8 * s["core_digital"]["Gr"]["configured_factor"]
        gains.append({"label": s["label"], "coarse": s["timing_raw"]["3012"],
                      "effective_gain_from_same_run_response": effective,
                      "core_code_gain_model": expected,
                      "gain_model_error_ev": math.log2(effective / expected) if effective and expected else None,
                      "reference_ei": canonical_ei * effective if canonical_ei and effective else None,
                      "status": "within_measured_response_domain" if inside else "outside_response_domain"})
    errors = [abs(r["gain_model_error_ev"]) for r in gains if r["gain_model_error_ev"] is not None and r["label"] != "auto_gain_0220"]
    normal_points = []
    for name, s in by_label.items():
        if name == "auto_normal_base" or re.fullmatch(r"auto_normal_\d+", name):
            normal_points.append((float(meters[s["phase"]]["linear_luma"]), s["timing_raw"]["3012"], name))
    normal_points.sort()
    normal_ei = None
    normal_q = None
    if len(normal_points) >= 3:
        normal_q = np.polynomial.polynomial.polyfit([p[0] for p in normal_points], [p[1] for p in normal_points], 2).tolist()
        c_normal = float(np.polynomial.polynomial.polyval(.18, normal_q))
        normal_ei = dose / (c_normal * row_seconds) if dose else None
    valid = (light_drift <= .125 and pair and phone_drift <= .125
             and len(errors) == 7 and max(errors) <= .25)
    return {"status": "drift_bracketed_reference_model" if valid else "provisional_or_failed_drift_bracket",
            "equation": "canonical Ceq=a0+a1*L+a2*L^2; native-WB normal response is fitted separately",
            "coefficients": q.tolist(), "training_luma_coarse_label": training,
            "natural_light_anchors": anchor_rows, "max_anchor_drift_ev": light_drift,
            "phone_reference_pair": pair, "phone_reference_drift_ev": phone_drift,
            "frame_period_seconds": period, "row_seconds_model": row_seconds,
            "target_coarse_model": target, "reference_gain_ei": canonical_ei,
            "native_wb_normal_coefficients": normal_q, "native_wb_normal_reference_ei": normal_ei,
            "gain_rows": gains, "gain_model_max_error_ev": max(errors) if errors else None,
            "reference_model_verified": valid, "absolute_iso_calibrated": False,
            "target_slightly_above_measured_luma": .18 > training[-1][0],
            "timing_note": "coarse/cadence model; fine and optical timing remain separately stated"}


def exposure_reference_values(reference, batch):
    """External photographic exposure, never an internal brightness target.

    Preserve each meter and shutter interval separately. EV100 describes the
    reported scene exposure; it does not certify this camera's ISO or shutter.
    """
    if reference is None:
        return []
    readings = reference.get("readings", [reference])
    values = []
    for reading in readings:
        iso, aperture = float(reading["iso"]), float(reading["f_number"])
        shutters = reading.get("shutter_seconds_interval")
        if shutters is None:
            shutters = [reading["shutter_seconds"]] * 2
        if len(shutters) != 2:
            raise ValueError("external shutter interval must have two bounds")
        lo, hi = map(float, shutters)
        if not all(math.isfinite(v) and v > 0 for v in (iso, aperture, lo, hi)) or lo > hi:
            raise ValueError("invalid photographic exposure reference")
        values.append({"meter": reading.get("meter", "external"), "iso": iso,
                       "f_number": aperture, "shutter_seconds_interval": [lo, hi],
                       "ev100_interval": [math.log2(aperture ** 2 / hi * 100 / iso),
                                          math.log2(aperture ** 2 / lo * 100 / iso)],
                       "reported_cct_kelvin": reading.get("cct_kelvin"),
                       "batch_bound": bool(batch and reference.get("illumination_batch") == batch),
                       "timing": reference.get("timing", "unspecified"),
                       "camera_iso_calibrated": False})
    return values


def analyse(directory, output, reference=None, batch=None):
    directory, output = Path(directory), Path(output)
    declarations = manifest_records(directory, "EXPOSURE_AUTOMATION")
    if len(declarations) != 1 or declarations[0].get("source") != "exposure-auto" or declarations[0].get("contract") not in ("1", "2", "3", "4", "5", "6"):
        raise ValueError("unknown/missing hardware automation contract")
    declaration = declarations[0]
    version = int(declaration["contract"])
    fps = declaration.get("fps")
    if fps not in ("15", "5") or (version < 3 and fps != "15"):
        raise ValueError("unregistered timing profile")
    upper = 1585 if fps == "5" else 528
    expected = {"max_stages": "18" if version >= 2 else "15", "normal_steps": "3", "sensor": "1", "fps": fps,
                "coarse_min": "1", "coarse_max": str(upper), "raw_locked": "1", "retry": "none"}
    if any(declaration.get(k) != v for k, v in expected.items()):
        raise ValueError("hardware automation contract was widened")
    output.mkdir(parents=True, exist_ok=True)
    report = decode(directory,frame_scope=f"exposure-auto/v{version}" if version>=3 else "calibration-legacy")
    if len(report["stages"]) > int(expected["max_stages"]):
        raise ValueError("stage budget exceeded")
    locked = [s for s in report["stages"] if s["kind"] != "seed"]
    evidence_ok = bool(locked) and all(s["proof_status"] == "verified_locked_sample" for s in locked)
    meters = manifest_records(directory, "AUTO_METER")
    meter_by_phase = {int(m["phase"]): m for m in meters}
    if len(meter_by_phase) != len(meters):
        raise ValueError("duplicate meter phase")
    if all(s["proof_status"] == "verified_locked_sample" for s in locked):
        ends=manifest_records(directory,"AUTO_END")
        partial=len(ends)!=1 or ends[0].get("complete")!="1"
        verify_registered_plan(report["stages"], meter_by_phase, version,upper,partial)
    images = []
    for stage in locked:
        phase = stage["phase"]
        raw = directory / f"s1-p{phase:03d}-{stage['label']}.yuyv"
        m = meter_by_phase.get(phase)
        if stage["proof_status"] != "verified_locked_sample" or not raw.exists() or not m or m["complete"] != "1":
            images.append({"phase": phase, "label": stage["label"], "status": "missing_or_rejected_evidence"})
            evidence_ok = False
            continue
        rgb = yuyv_rgb(raw, int(m["matrix"]))
        source_meter = meter_rgb(rgb)
        image = Image.fromarray(np.rint(rgb * 255).astype(np.uint8))
        jpeg = output / (raw.stem + ".jpg")
        image.save(jpeg, quality=95, subsampling=0)
        with Image.open(jpeg) as decoded:
            jpeg_meter = meter_rgb(np.asarray(decoded.convert("RGB"), dtype=np.float64) / 255)
        native = float(m["last_linear_luma"])
        native_error = abs(source_meter["linear_mean"] - native)
        jpeg_error_ev = math.log2(max(jpeg_meter["linear_mean"], 1e-12) / max(source_meter["linear_mean"], 1e-12))
        checks = native_error < 1e-7 and (abs(jpeg_error_ev) <= .05 or source_meter["linear_mean"] < .005)
        images.append({"phase": phase, "label": stage["label"], "source": raw.name,
                       "source_sha256": hashlib.sha256(raw.read_bytes()).hexdigest(), "jpeg": jpeg.name,
                       "source_meter": source_meter, "jpeg_meter": jpeg_meter,
                       "native_last_frame_luma": native, "native_luma_abs_error": native_error,
                       "native_mean_12_frames": float(m["linear_luma"]), "jpeg_error_ev": jpeg_error_ev,
                       "status": "verified" if checks else "metering_mismatch"})
        evidence_ok = evidence_ok and checks
    ending = manifest_records(directory, "AUTO_END")
    normal = manifest_records(directory, "AUTO_NORMAL")
    complete = (len(ending) == 1 and ending[0].get("complete") == "1" and ending[0].get("seed_restored") == "1"
                and ending[0].get("gain_points") == "6" and ending[0].get("digital_points") == "2"
                and int(ending[0].get("stages", "0")) == len(report["stages"]))
    if complete:
        expected_gain = {f"auto_gain_{code:04x}" for code in (0x210, 0x218, 0x220, 0x228, 0x230, 0x23c)}
        actual_gain = {s["label"] for s in locked if s["kind"] == ("auto_gain" if version >= 2 else "code")}
        complete = actual_gain == expected_gain and {s["label"] for s in locked if s["kind"] == ("auto_digital" if version >= 2 else "digital")} == {"auto_digital_2", "auto_digital_4"}
    reference_data = json.loads(Path(reference).read_text()) if reference else None
    reference_status = "awaiting_current_scene_reference"
    if reference_data is not None:
        reference_status = "bound_to_batch" if batch and reference_data.get("illumination_batch") == batch else "unbound_batch_not_used_for_absolute_calibration"
    result = {"contract": f"exposure-auto/v{version}", "source": str(directory.resolve()), "illumination_batch": batch,
              "hardware_evidence_verified": evidence_ok, "registered_cycle_complete": complete,
              "normal_exposure": normal[0] if len(normal) == 1 else {"outcome": "incomplete"},
              "reference_status": reference_status, "reference": reference_data,
              "photographic_exposure_references": exposure_reference_values(reference_data, batch),
              "absolute_iso_calibrated": False,
              "jpeg_comparison_kind": "derived_from_same_YUYV; validates metering/encoding, not an independent radiometric reference",
              "images": images, "decoded_controls": report,
              "notes": ["configured digital multipliers remain separate for sensor core and ISP",
                        "normal convergence and exploration completeness are separate verdicts",
                        "a reference from another illumination batch is retained but not silently applied"]}
    if evidence_ok and complete:
        result["exposure_model"] = (fit_natural_light_model if version >= 2 else fit_reference_model)(directory, locked, meter_by_phase, reference_data, batch)
    (output / "session-analysis.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    write_report(report, output / "controls")
    rows = ["# 硬件自动曝光与探索报告", "",
            f"硬件证据核验：{evidence_ok}；已登记采集周期完整：{complete}。",
            f"正常曝光结果：{result['normal_exposure'].get('outcome', 'incomplete')}。",
            f"参考参数状态：{reference_status}。绝对ISO未宣称完成。", ""]
    if result["photographic_exposure_references"]:
        rows += ["## 外部曝光基準", "",
                 "EV100 = log2(N² / t × 100 / ISO)。各測光工具分列，區間不取虛構中點。",
                 "曝光基準不是內部亮度0.18，也不代表掌機已達到ISO100。", "",
                 "|測光工具|ISO|光圈|快門秒數區間|EV100區間|報告色溫K|",
                 "|---|---:|---:|---|---|---:|"]
        for value in result["photographic_exposure_references"]:
            lo, hi = value["shutter_seconds_interval"]
            evlo, evhi = value["ev100_interval"]
            rows.append(f"|{value['meter']}|{value['iso']:g}|f/{value['f_number']:g}|{lo:g}–{hi:g}|{evlo:.4f}–{evhi:.4f}|{value['reported_cct_kelvin']}|")
        rows += ["", "本輪為採集後參考；不證明採集期間自然光恆定。色溫獨立記錄，不作曝光補償。", ""]
    rows += ["|Phase|目的|原始线性均值|JPEG测光差EV|验证|", "|---:|---|---:|---:|---|"]
    for image in images:
        if "source_meter" in image:
            rows.append(f"|{image['phase']}|{image['label']}|{image['source_meter']['linear_mean']:.6f}|{image['jpeg_error_ev']:.5f}|{image['status']}|")
        else:
            rows.append(f"|{image['phase']}|{image['label']}|—|—|{image['status']}|")
    rows += ["", "JPEG来自同一YUYV，只验证编码及测光一致性，不冒充独立物理校准。",
             "每张JPEG可直接查看；原始YUYV与CRC回收记录保留。", ""]
    if "exposure_model" in result:
        model = result["exposure_model"]
        rows += ["## 同批次曝光模型", "", f"状态：{model['status']}。",
                 f"模型：{model.get('equation', '证据不足')}。"]
        if "coefficients" in model:
            rows += [f"系数：{model['coefficients']}。",
                     f"模型验证：{model.get('response_max_holdout_ev', model.get('gain_model_max_error_ev'))} EV。",
                     f"当前外部参考下基准EI：{model['reference_gain_ei']}；自然光只有后测参考，仍是暂定锚点。",
                     "", "|增益设置|同批响应推算有效倍率|参考EI|状态|", "|---|---:|---:|---|"]
            for g in model["gain_rows"]:
                multiplier = f"{g['effective_gain_from_same_run_response']:.4f}" if g["effective_gain_from_same_run_response"] is not None else "待补测"
                ei = f"{g['reference_ei']:.1f}" if g["reference_ei"] is not None else "待补测"
                rows.append(f"|{g['label']}|{multiplier}|{ei}|{g['status']}|")
            rows += ["", "目标0.18略高于本轮最高实测亮度，目标曝光含小幅模型外推；最低两个增益点不外推。",
                     "自然光需前后测光和固定参考幀夹测；不得把本轮后测读数当作全程恒定照明。", ""]
    (output / "RESULTS.md").write_text("\n".join(rows))
    return result


def run(args):
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    arguments=ARGS+(f"--automation-fps={args.fps}",)
    request = {"contract": "exposure-auto/v6", "device": args.ip, "arguments": arguments,
               "illumination_batch": args.batch, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
               "readiness_deadline_seconds": args.deadline, "readiness_poll_seconds": 1,
               "measurement_retries": 0, "started_unix": time.time()}
    source = out / "source"
    source.mkdir()
    request["source_sha256"] = {}
    for path in sorted((PROBE / "probes/exposure").glob("*")):
        if path.is_file():
            shutil.copyfile(path, source / path.name)
            request["source_sha256"][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    shutil.copyfile(binary, out / binary.name)
    if args.reference:
        shutil.copyfile(args.reference, out / "reference-before.json")
        shutil.copyfile(args.reference, out / "reference.json")
        request["reference_before_sha256"] = hashlib.sha256(args.reference.read_bytes()).hexdigest()
    (out / "deployment.json").write_text(json.dumps(request, indent=2) + "\n")
    # No empty NetLoader probe: the one connection carries a complete upload.
    deploy(args.ip, binary, arguments)
    print("Hardware feedback/exploration launched; waiting for frozen evidence.", flush=True)
    deadline = time.monotonic() + args.deadline
    while time.monotonic() < deadline:
        try:
            status, _, data = get(args.ip, "/info")
            if status == 200 and data.startswith(b"FILMRAM 2\n"):
                _,entries=parse_index(data)
                (out / "capture-ready.json").write_text(json.dumps({"observed_unix": time.time(), "index_sha256": hashlib.sha256(data).hexdigest()}) + "\n")
                quick=out/'quick';quick.mkdir()
                for name in ('manifest.txt','calibration_plan.csv','frame_burst.csv'):
                    entry=next(e for e in entries if e['name']==name)
                    status,headers,body=get(args.ip,f"/file/{entry['id']}",{'Range':f"bytes=0-{entry['bytes']-1}"})
                    if status!=206 or len(body)!=entry['bytes'] or f'{zlib.crc32(body):08x}'!=entry['crc32']:
                        raise ValueError('quick summary CRC/range mismatch')
                    (quick/name).write_bytes(body)
                print((quick/'manifest.txt').read_text().split('AUTO_NORMAL ',1)[-1].split('\n',1)[0],flush=True)
                print("CAPTURE COMPLETE: take the after-run external meter reading now.", flush=True)
                break
        except (OSError, http.client.HTTPException):
            pass  # Registered read-only readiness polling, never repeats capture.
        time.sleep(1)
    else:
        raise TimeoutError("hardware readiness deadline; no redeployment or capture retry")
    retrieve(args.ip, out / "recovered")
    result = analyse(out / "recovered", out / "analysis", args.reference, args.batch)
    print(json.dumps({k: result[k] for k in ("hardware_evidence_verified", "registered_cycle_complete", "normal_exposure", "reference_status")}, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    live = sub.add_parser("run")
    live.add_argument("ip")
    live.add_argument("--binary", type=Path, default=PROBE / "FilmExposureAuto.3dsx")
    live.add_argument("--out", type=Path, required=True)
    live.add_argument("--batch", required=True)
    live.add_argument("--deadline", type=int, default=900)
    live.add_argument("--fps",type=int,choices=[5,15],default=5)
    live.add_argument("--reference", type=Path)
    offline = sub.add_parser("analyse")
    offline.add_argument("directory", type=Path)
    offline.add_argument("--out", type=Path, required=True)
    offline.add_argument("--reference", type=Path)
    offline.add_argument("--batch")
    recovery = sub.add_parser("recover", help="resume frozen evidence only; never deploy or capture")
    recovery.add_argument("directory", type=Path, help="existing session containing deployment and RAM fingerprint")
    recovery.add_argument("--reference", type=Path)
    args = parser.parse_args()
    if args.command == "run":
        if not 1 <= args.deadline <= 1800:
            parser.error("deadline must be 1..1800 seconds")
        result = run(args)
    elif args.command == "analyse":
        result = analyse(args.directory, args.out, args.reference, args.batch)
    else:
        record = json.loads((args.directory / "deployment.json").read_text())
        fingerprint = args.directory / "recovered/ram-source.json"
        if not fingerprint.exists():
            raise ValueError("no existing frozen snapshot fingerprint; cannot resume")
        attempt = {"contract": "frozen-resume-v2", "started_unix": time.time(),
                   "chunk_bytes": 16384, "request_gap_seconds": .1, "capture_repeated": False,
                   "max_attempts_per_range": 4, "max_transient_failures_total": 32}
        (args.directory / f"recovery-attempt-{time.time_ns()}.json").write_text(json.dumps(attempt, indent=2) + "\n")
        retrieve(record["device"], args.directory / "recovered", chunk=16384, request_gap=.1,recovery_policy="frozen-resume-v2")
        reference = args.reference or (args.directory / "reference.json" if (args.directory / "reference.json").exists() else None)
        result = analyse(args.directory / "recovered", args.directory / "analysis", reference, record["illumination_batch"])
    return 0 if result["hardware_evidence_verified"] and result["registered_cycle_complete"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
