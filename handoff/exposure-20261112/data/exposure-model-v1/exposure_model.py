"""Offline Sensor1 calibration model. Does not open/control any camera.

Fit only the original 15fps sweep. All 30fps samples are external validation.
Units: C is coarse integration code; u is processed center-Y divided by 255.
EI is explicitly a nominal-f/2.8 reference-output model, not measured ISO.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys

import numpy as np

ROOT = Path(__file__).resolve().parent
BASE = ROOT.parent
FILM = BASE.parents[3]
PROBE_TOOLS = FILM / "build/worktrees/camera-deep-probe/tools"
sys.path.insert(0, str(PROBE_TOOLS))
from calibration_report import summarize

# Offline analysis boundary: source files are read-only. No work submission,
# queues, device I/O, automatic retries, runtime configuration or deployment.
SOURCES = {
    "integrations": BASE / "offline-full-01/analysis-prefix",
    "gain": BASE / "ram-codes-fine-01/recovered",
    "slow": BASE / "slow-part-01/recovered",
    "digital": BASE / "digital-part-01/recovered",
}
# The original partial full-run CSV has a separate, explicitly trimmed analysis
# copy. PGM originals remain in recovered; never fabricate/copy missing images.
INTEGRATION_IMAGES = BASE / "offline-full-01/recovered"
OUTPUT_TARGET_Y = 118.0  # Explicit output convention, never inferred gray reflectance.
MAX_RESPONSE_ERROR_EV = 0.25  # Engineering acceptance criterion, chosen for this review.
MIN_VALID_Y = 20.59838867  # Lowest interior 15fps point with leave-one-point-out evidence.
MAX_VALID_Y = 212.53177897
MODEL_NAME = "sensor1-normal-photo-center-y-inverse-cubic-v1"


def constant(stage, address):
    field = stage["registers"][address]
    values = field["values"]
    if not field["complete"] or not field["constant"] or len(set(values)) != 1:
        raise ValueError(f"incomplete/drifting register {address}")
    return values[0]


def checked(stage):
    if not stage["complete"] or not stage["locked_controls_held"] or stage["measured_rows"] != 12:
        raise ValueError("stage has no complete held-control proof")
    return stage


def value(coefficients, y, validate=True):
    if not math.isfinite(y) or (validate and not MIN_VALID_Y <= y <= MAX_VALID_Y):
        raise ValueError("Y outside independently checked response domain")
    return float(np.polynomial.polynomial.polyval(y / 255, coefficients))


def fit(points):
    if len(points) < 4:
        raise ValueError("cubic requires four distinct control points")
    y = np.array([p[1] / 255 for p in points])
    c = np.array([p[0] for p in points])
    if len(set(c)) != len(c) or not np.all(np.diff(y) > 0):
        raise ValueError("non-distinct/non-monotonic response")
    return np.polynomial.polynomial.polyfit(y, c, 3).tolist()


def cadence(directory, rate):
    with (directory / "frame_burst.csv").open() as file:
        rows = [r for r in csv.DictReader(file) if r["rate"] == str(rate)]
    if len(rows) != 24 or any(r["wait_result"] != "00000000" or r["receive_result"] != "00000000"
                              or r["received"] != "1" or r["buffer_error"] != "0" for r in rows):
        raise ValueError("incomplete DMA timing burst")
    rows.sort(key=lambda r: int(r["frame"]))
    times = [int(r["time_ms"]) for r in rows]
    intervals = [b - a for a, b in zip(times[2:], times[3:])]
    return statistics.mean(intervals) / 1000


def roi_rows(directory, phase):
    with (directory / "roi.csv").open() as file:
        return [r for r in csv.DictReader(file)
                if int(r["phase"]) == phase and r["tile"] == "0" and 8 <= int(r["frame"]) <= 19]


def raw_image_check(directory, phase, label, rows):
    # Independent pixel length and center mean, rather than trusting report.json.
    path = directory / f"s1-p{phase:03d}-{label}.pgm"
    content = path.read_bytes()
    header = b"P5\n640 480\n255\n"
    if not content.startswith(header) or len(content) != len(header) + 640 * 480:
        raise ValueError("bad PGM header or pixel length")
    pixels = np.frombuffer(content[len(header):], dtype=np.uint8).reshape(480, 640)
    last = max(rows, key=lambda r: int(r["frame"]))
    delta = float(pixels[192:288, 256:384].mean()) - float(last["mean"])
    if abs(delta) > 1e-7:
        raise ValueError("PGM/recorded last-frame ROI mismatch")
    return {"file": str(path.relative_to(BASE)), "last_frame_mean_error": delta}


def nominal_ei(coefficients, y, coarse_time, meter_time, reference_iso, target_y=OUTPUT_TARGET_Y):
    # Inverse response maps output to equivalent coarse dose in the TRAIN scene.
    # Its use in the meter-02 scene explicitly assumes transfer invariance.
    return reference_iso * meter_time / coarse_time * value(coefficients, y) / value(coefficients, target_y)


def solve_fixed_gain(coefficients, current_coarse, observed_y, target_y):
    """Offline exposure decision, fixed full gain vector and unchanged mode.

    Returns a model prediction, not a claim that the new code was captured.
    No rounding, clipping, writes, automatic gain changes or retry behavior.
    """
    if not math.isfinite(current_coarse) or current_coarse <= 0:
        raise ValueError("invalid current integration")
    return current_coarse * value(coefficients, target_y) / value(coefficients, observed_y)


def plan_observed_setting(result, fps, current_coarse, observed_y, target_y):
    """Offline plan selecting one observed full combination; no hardware I/O."""
    if fps not in (15, 30):
        raise ValueError("profile has no complete cross-fps response validation")
    frame_lines = {15: 704, 30: 518}[fps]
    candidates = [r for r in result["observed_complete_controls"]
                  if r["dataset"] == "integrations" and r["frame_lines"] == frame_lines
                  and r["four_channel_codes"] == [544, 586, 549, 544]
                  and r["digital_3032"] == 256
                  and MIN_VALID_Y <= r["center_y"] <= MAX_VALID_Y]
    if current_coarse not in {r["coarse"] for r in candidates}:
        raise ValueError("current setting outside supported measured combinations")
    predicted = solve_fixed_gain(result["response"]["coefficients"], current_coarse, observed_y, target_y)
    if not min(r["coarse"] for r in candidates) <= predicted <= max(r["coarse"] for r in candidates):
        raise ValueError("requested exposure outside supported control interval")
    selected = min(candidates, key=lambda r: abs(math.log2(r["coarse"] / predicted)))
    return {"offline_plan_only": True, "predicted_unmeasured_coarse": predicted,
            "selected_observed_combination": selected,
            "quantization_ev": math.log2(selected["coarse"] / predicted),
            "hardware_applied": False, "continuous_preview_verified": False,
            "requirement": "same full gain vector, digital gain, mode and output response"}


def build():
    # Re-run existing strict verifier from original CSV/readbacks; do not trust saved summaries.
    reports = {name: summarize(directory) for name, directory in SOURCES.items()}
    for name in ("gain", "slow", "digital"):
        if not reports[name]["measurements_complete"]:
            raise ValueError(f"incomplete dataset {name}")
    phases = reports["integrations"]["phases"]
    train = []
    image_checks = []
    for phase, stage in phases.items():
        if stage["label"].startswith("cal_r0_t") and stage["complete"] and stage["center_mean"] > 2:
            checked(stage)
            c = constant(stage, "3012")
            train.append((c, stage["center_mean"], int(phase)))
            image_checks.append(raw_image_check(INTEGRATION_IMAGES, int(phase), stage["label"],
                                               roi_rows(SOURCES["integrations"], int(phase))))
    train.sort()
    coefficients = fit(train)
    derivative = np.polynomial.polynomial.polyder(coefficients)
    if min(np.polynomial.polynomial.polyval(np.linspace(MIN_VALID_Y / 255, MAX_VALID_Y / 255, 1000), derivative)) <= 0:
        raise ValueError("inverse response is not monotone")

    # Internal model selection diagnostic: omit an ENTIRE exposure point each time.
    leave_one_out = []
    for i in range(1, len(train) - 1):
        c, y, phase = train[i]
        held_fit = fit(train[:i] + train[i + 1:])
        predicted = value(held_fit, y)
        leave_one_out.append({"phase": phase, "actual_coarse": c, "predicted_coarse": predicted,
                              "error_ev": math.log2(predicted / c)})

    period15 = cadence(SOURCES["integrations"], 0)
    period30 = cadence(SOURCES["integrations"], 8)
    row15 = period15 / 704
    row30 = period30 / 518
    cross_fps = []
    for phase, stage in phases.items():
        if stage["label"].startswith("cal_r8_t") and stage["complete"] and MIN_VALID_Y <= stage["center_mean"] <= MAX_VALID_Y:
            checked(stage)
            if [constant(stage, k) for k in ("3056", "3058", "305a", "305c")] != [544, 586, 549, 544]:
                raise ValueError("cross-fps controls differ")
            actual = constant(stage, "3012") * row30
            predicted = value(coefficients, stage["center_mean"]) * row15
            cross_fps.append({"phase": int(phase), "label": stage["label"], "actual_model_seconds": actual,
                              "predicted_seconds_from_y": predicted, "error_ev": math.log2(predicted / actual)})
            image_checks.append(raw_image_check(INTEGRATION_IMAGES, int(phase), stage["label"],
                                               roi_rows(SOURCES["integrations"], int(phase))))

    # Temporal holdout: first six frames fit; last six frames never fit.
    first_half = []
    for c, _, phase in train:
        rows = roi_rows(SOURCES["integrations"], phase)
        first_half.append((c, statistics.median(float(r["mean"]) for r in rows if int(r["frame"]) < 14)))
    temporal_fit = fit(first_half)
    temporal_errors = []
    for c, _, phase in train:
        for r in roi_rows(SOURCES["integrations"], phase):
            if int(r["frame"]) >= 14 and MIN_VALID_Y <= float(r["mean"]) <= MAX_VALID_Y:
                temporal_errors.append(math.log2(value(temporal_fit, float(r["mean"])) / c))

    meter_path = BASE / "external-meter-reference-02.json"
    meter = json.loads(meter_path.read_text())
    gain = []
    restore_eis = []
    for phase, stage in reports["gain"]["phases"].items():
        if stage["kind"] not in ("code", "code_baseline", "restore"):
            continue
        checked(stage)
        c = constant(stage, "3012")
        if c != 7 or constant(stage, "300a") != 704 or constant(stage, "300c") != 843:
            raise ValueError("gain scan timing mismatch")
        codes = [constant(stage, k) for k in ("3056", "3058", "305a", "305c")]
        if len(set(codes)) != 1 or constant(stage, "3032") != 256:
            raise ValueError("gain scan is not equal-channel / digital1x")
        y = stage["center_mean"]
        rows = roi_rows(SOURCES["gain"], int(phase))
        image_checks.append(raw_image_check(SOURCES["gain"], int(phase), stage["label"], rows))
        supported = MIN_VALID_Y <= y <= MAX_VALID_Y
        ei = nominal_ei(coefficients, y, c * row15, meter["shutter_seconds"], meter["iso"]) if supported else None
        if stage["kind"] != "code":
            if ei is not None:
                restore_eis.append(ei)
            continue
        # Sensitivity scenario only: no claim that fine+coarse formula is proven here.
        fine_plus_time = (c + 633 / 843) * row15
        gain.append({"code": f"0x{codes[0]:04X}", "core_302a": constant(stage, "302a"),
                     "phase": int(phase), "center_y": y, "model_domain_supported": supported,
                     "nominal_reference_ei": ei,
                     "fine_plus_sensitivity_ei": ei * c * row15 / fine_plus_time if ei else None,
                     "status": "conditional_meter02_EI_not_absolute_ISO" if supported else "rejected_dark_domain"})

    reference_gain = next(r["nominal_reference_ei"] for r in gain if r["code"] == "0x0220")
    for row in gain:
        row["relative_effective_gain_to_0220"] = row["nominal_reference_ei"] / reference_gain if row["nominal_reference_ei"] else None
        row["relative_ev_to_0220"] = math.log2(row["relative_effective_gain_to_0220"]) if row["relative_effective_gain_to_0220"] else None

    # Test the actual exposure-decision ratio on all six ordered pairs from
    # the THREE independently held-out 30fps points (not six new experiments).
    decision_checks = []
    for origin in cross_fps:
        a = phases[str(origin["phase"])]
        for destination in cross_fps:
            if origin["phase"] == destination["phase"]:
                continue
            b = phases[str(destination["phase"])]
            predicted = solve_fixed_gain(coefficients, constant(a, "3012"), a["center_mean"], b["center_mean"])
            expected = constant(b, "3012")
            decision_checks.append({"from_phase": origin["phase"], "to_phase": destination["phase"],
                                    "predicted_coarse": predicted, "actual_coarse": expected,
                                    "error_ev": math.log2(predicted / expected)})

    # Exposure reciprocity across new illumination / slightly different color gains:
    # fit ONE scale using the independent 5fps BASELINE, then predict sweep points.
    # Do not silently treat domain failures as passes.
    slow_phases = reports["slow"]["phases"]
    slow_base = next(s for s in slow_phases.values() if s["kind"] == "normalize")
    slow_scale = constant(slow_base, "3012") / value(coefficients, slow_base["center_mean"])
    slow_results = []
    for phase, stage in slow_phases.items():
        if stage["kind"] != "integration":
            continue
        y = stage["center_mean"]
        if MIN_VALID_Y <= y <= MAX_VALID_Y:
            estimated = slow_scale * value(coefficients, y)
            error = math.log2(estimated / constant(stage, "3012"))
            status = "within_engineering_tolerance" if abs(error) <= MAX_RESPONSE_ERROR_EV else "failed"
        else:
            estimated, error, status = None, None, "outside_response_domain"
        slow_results.append({"phase": int(phase), "coarse": constant(stage, "3012"), "y": y,
                             "predicted_coarse": estimated, "error_ev": error, "status": status})

    # Fully observed combinations only, directly from strict raw proofs.
    controls = []
    for dataset, report in reports.items():
        for phase, stage in report["phases"].items():
            if stage["kind"] not in ("integration", "normalize", "code", "digital") or not stage["complete"]:
                continue
            checked(stage)
            controls.append({"dataset": dataset, "phase": int(phase), "label": stage["label"],
                             "coarse": constant(stage, "3012"),
                             "four_channel_codes": [constant(stage, k) for k in ("3056", "3058", "305a", "305c")],
                             "frame_lines": constant(stage, "300a"), "line_pck": constant(stage, "300c"),
                             "digital_3032": constant(stage, "3032"), "center_y": stage["center_mean"],
                             "measured_frames": 12})

    validation = {
        "criterion_ev": MAX_RESPONSE_ERROR_EV,
        "leave_one_exposure_out": leave_one_out,
        "cross_30fps_unfitted": cross_fps,
        "temporal_holdout_frames": len(temporal_errors),
        "temporal_holdout_max_abs_ev": max(map(abs, temporal_errors)),
        "temporal_holdout_rmse_ev": math.sqrt(statistics.mean(v * v for v in temporal_errors)),
        "slow_baseline_scaled_unfitted": slow_results,
        "exposure_decision_30fps_pairs": decision_checks,
        "raw_pgm_roi_checks": image_checks,
    }
    validation["15fps_response_and_30fps_transfer_pass"] = (
        len(cross_fps) == 3 and len(leave_one_out) == 3
        and max(abs(p["error_ev"]) for p in leave_one_out + cross_fps) <= MAX_RESPONSE_ERROR_EV
        and validation["temporal_holdout_max_abs_ev"] <= MAX_RESPONSE_ERROR_EV)
    input_hashes = {}
    for directory in SOURCES.values():
        for filename in ("calibration_plan.csv", "frames.csv", "reads.csv", "roi.csv", "noise.csv", "manifest.txt", "frame_burst.csv"):
            path = directory / filename
            input_hashes[str(path.relative_to(BASE))] = hashlib.sha256(path.read_bytes()).hexdigest()
    input_hashes[str(meter_path.relative_to(BASE))] = hashlib.sha256(meter_path.read_bytes()).hexdigest()
    for name in ("reads.csv", "frames.csv", "calibration_plan.csv", "manifest.txt"):
        path = INTEGRATION_IMAGES / name
        input_hashes[str(path.relative_to(BASE))] = hashlib.sha256(path.read_bytes()).hexdigest()
    for check in image_checks:
        path = BASE / check["file"]
        input_hashes[check["file"]] = hashlib.sha256(path.read_bytes()).hexdigest()

    result = {
        "schema": MODEL_NAME,
        "boundary": "offline_analysis_only_no_camera_or_runtime_mutation",
        "sensor": 1,
        "response": {"equation": "Ceq15 = a0 + a1*u + a2*u^2 + a3*u^3; u=center_Y/255",
                     "coefficients": coefficients, "valid_y": [MIN_VALID_Y, MAX_VALID_Y],
                     "training_points_coarse_y_phase": train,
                     "scope": "normal-photo measured center-ROI response, original meter01 scene; not universal radiometry"},
        "fixed_gain_exposure_decision": {
            "formula": "Cnext=Ccurrent*Ceq15(Ytarget)/Ceq15(Yobserved)",
            "requirements": "unchanged complete four-channel gain vector, digital gain, mode and output response",
            "result_kind": "predicted coarse integration, not newly hardware-tested setting",
            "no_hardware_actions": True,
        },
        "timing": {"15fps_period_seconds": period15, "30fps_period_seconds": period30,
                   "15fps_row_seconds": row15, "30fps_row_seconds": row30,
                   "formula": "t_coarse=C*measured_DMA_frame_period/frame_lines",
                   "absolute_optical_time_verified": False,
                   "fine_3014": 633, "fine_offset_included": False},
        "meter02_reference": meter,
        "reference_ei_model": {
            "formula": "EI=100*(0.008/t_coarse)*Ceq15(Y)/Ceq15(118)",
            "nominal_aperture": 2.8, "target_y_convention": OUTPUT_TARGET_Y,
            "absolute_iso_verified": False,
            "cross_illumination_transfer_verified": False,
            "assumptions": ["nominal f/2.8 and the supplied meter02 reference apply",
                            "meter02 scan uses same output-response function as the calibrated meter01 curve",
                            "center-mean response may be used as a scalar exposure proxy",
                            "Y118 is the chosen output criterion; it is not a measured gray-card reflectance"],
            "fine_plus_scenario": "EI adjusted by 7/(7+633/843); sensitivity only, not an optical calibration",
            "baseline_0220_ei_repeats": restore_eis,
            "baseline_0220_ei_span_ev": math.log2(max(restore_eis) / min(restore_eis)),
            "0220_target_vs_baseline_median_ev": math.log2(reference_gain / statistics.median(restore_eis)),
            "gain_rows": gain,
        },
        "observed_complete_controls": controls,
        "validation": validation,
        "input_sha256": input_hashes,
        "limitations": ["response validation is not validation of absolute ISO or optical shutter seconds",
                        "dark/saturated response is rejected, not extrapolated",
                        "a single scene-scale change was checked only at supported 5fps points",
                        "no arbitrary shutter/gain Cartesian product is presented as tested",
                        "single-frame restart capture only; no continuous-preview claim",
                        "original incomplete full run remains incomplete; only strictly complete stages are used"],
    }
    return result


def save(result):
    (ROOT / "model.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    rows = result["reference_ei_model"]["gain_rows"]
    with (ROOT / "reference-ei.csv").open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    v = result["validation"]
    report = ["# Sensor1 曝光模型与验证交付", "",
              "状态：15fps积分响应及30fps同场景迁移通过本轮±0.25EV工程判据。绝对ISO、光学快门秒数及跨照明EI迁移未验证。",
              "", "## 可执行模型", "",
              "令u=中心Y均值/255，15fps等效粗积分 C=a0+a1·u+a2·u²+a3·u³。",
              "系数：`" + json.dumps(result["response"]["coefficients"]) + "`。",
              f"输入域：Y={MIN_VALID_Y:.3f}–{MAX_VALID_Y:.3f}；暗端、过曝端拒绝外推。",
              "原始15fps积分点拟合，不使用30fps点拟合；不是套用sRGB，也不是跨批次拼接亮度曲线。",
              "固定全通道增益时，曝光决策为 Cnext=Ccurrent×C(Ytarget)/C(Yobserved)。",
              "solve_fixed_gain返回预测值，不写硬件，也不把未采样组合宣称为已验证。",
              "", "## 数值验证", "",
              "|检查|结果|", "|---|---|",
              f"|整档留出：16/64/176|最大绝对误差 {max(abs(p['error_ev']) for p in v['leave_one_exposure_out']):.4f} EV|",
              f"|30fps独立三点：64/129/388|最大绝对误差 {max(abs(p['error_ev']) for p in v['cross_30fps_unfitted']):.4f} EV|",
              f"|三点之间6个有向曝光决策复算|最大误差 {max(abs(p['error_ev']) for p in v['exposure_decision_30fps_pairs']):.4f} EV；仍只有3个独立实测点|",
              f"|前6帧拟合、后6帧验证|{v['temporal_holdout_frames']} 帧，最大 {v['temporal_holdout_max_abs_ev']:.4f} EV，RMSE {v['temporal_holdout_rmse_ev']:.4f} EV|",
              f"|PGM长度及像素ROI对CSV复核|{len(v['raw_pgm_roi_checks'])} 张通过|",
              "|证据来源|重新运行原始CSV/逐帧寄存器/AE/AWB严格验证；原件只读|",
              "", "## 外部参考下的条件性EI参数", "",
              "外部参考使用用户meter02：ISO100、f/2.8、1/125秒。输出判据明确选Y118。",
              "EI=100×(8ms/t7)×C(Y)/C(118)，t7为现有时序的粗积分项。",
              "下表数字是指定曝光模型下的参考参数，不是标准ISO测定。跨照明的响应不变性仍是前提。",
              "|四通道代码|中心Y|参考EI：粗积分模型|加入fine项的敏感性场景|状态|",
              "|---|---:|---:|---:|---|",]
    for row in rows:
        fmt = lambda x: f"{x:.1f}" if x is not None else "拒绝"
        report.append(f"|{row['code']}|{row['center_y']:.3f}|{fmt(row['nominal_reference_ei'])}|"
                      f"{fmt(row['fine_plus_sensitivity_ei'])}|{row['status']}|")
    report += ["", "fine列只用于量化尚未确定的时序偏移影响，两列不是误差界。",
               f"同批0x0220恢复样本的参考EI跨度为 {result['reference_ei_model']['baseline_0220_ei_span_ev']:.4f} EV；寄存器保持不等于光度完全不漂。",
               f"0x0220目标阶段与其基准/恢复中位数另有 {result['reference_ei_model']['0220_target_vs_baseline_median_ev']:.4f} EV 偏差，不能隐去。",
               "", "## 可用范围与失败边界", "",
               "完整实测设置逐行在model.json的observed_complete_controls，不允许任意交叉组合冒充已测。",
               "5fps独立批次只用baseline拟合一个场景倍率，积分扫描留作验证，结果如下：",
               "|粗积分|中心Y|预测误差EV|判定|", "|---:|---:|---:|---|"]
    for p in v["slow_baseline_scaled_unfitted"]:
        error = f"{p['error_ev']:.4f}" if p["error_ev"] is not None else "—"
        report.append(f"|{p['coarse']}|{p['y']:.3f}|{error}|{p['status']}|")
    report += ["", "## 复现", "", "`python3 exposure_model.py build`", "`python3 test_model.py`", "",
               "离线决策示例：`python3 exposure_model.py solve --fps 30 --current-coarse 64 --observed-y 57.326049805 --target-y 88.677205405`。",
               "预测粗积分122.21；从完整实测行中选129，量化差约0.078EV。此命令不连接或写入设备。", "",
               "边界分类：Film3DS离线模型拟合/原件验证；没有设备操作或运行时行为变更。",
               "Vision CONTRACT_MATRIX、运行时验证器及runtime_internal_fields均未修改。",
               "新增数值与负例检查见test_model.py；另运行既有探针calibration_report测试。",
               "Vision三条pytest未运行：本次不涉及其运行时边界，Film3DS不包含这些测试。",
               "本交付替代derived-ei-review中的未验证曲线试算；旧件保留供审计。", ""]
    (ROOT / "RESULTS.md").write_text("\n".join(report))
    print(json.dumps({"response_pass": v["15fps_response_and_30fps_transfer_pass"],
                      "leave_one_out_ev": [p["error_ev"] for p in v["leave_one_exposure_out"]],
                      "cross_fps_ev": [p["error_ev"] for p in v["cross_30fps_unfitted"]],
                      "temporal_max_ev": v["temporal_holdout_max_abs_ev"], "gain_rows": rows}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("build")
    solve = sub.add_parser("solve")
    solve.add_argument("--fps", type=int, choices=[15, 30], required=True)
    solve.add_argument("--current-coarse", type=int, required=True)
    solve.add_argument("--observed-y", type=float, required=True)
    solve.add_argument("--target-y", type=float, required=True)
    args = parser.parse_args()
    if args.command == "build":
        save(build())
    else:
        calibrated = json.loads((ROOT / "model.json").read_text())
        print(json.dumps(plan_observed_setting(calibrated, args.fps, args.current_coarse,
                                              args.observed_y, args.target_y), indent=2))
