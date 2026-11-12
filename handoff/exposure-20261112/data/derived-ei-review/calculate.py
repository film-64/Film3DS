"""Offline conditional EI estimates; no hardware access or runtime changes.

Uses processed center-Y statistics, NOT RAW or measured sensor ISO.
The two empirical curves are alternative model fits, not uncertainty bounds.
"""
import bisect
import csv
import hashlib
import json
import math
from pathlib import Path

OUT = Path(__file__).resolve().parent
BASE = OUT.parent
INPUTS = {
    "integration": BASE / "offline-full-01/analysis-prefix/calibration-analysis.json",
    "gain": BASE / "ram-codes-fine-01/recovered/calibration-analysis.json",
    "meter": BASE / "external-meter-reference-02.json",
}
data = {key: json.loads(path.read_text()) for key, path in INPUTS.items()}


def reg(stage, key):
    entry = stage["registers"][key]
    assert entry["complete"] and entry["constant"]
    assert len(set(entry["values"])) == 1
    return entry["values"][0]


def inverse_curve(points, y):
    """Piecewise linear Y versus log(coarse); forbid extrapolation."""
    points = sorted(points)
    assert all(b[1] > a[1] for a, b in zip(points, points[1:]))
    assert points[0][0] <= y <= points[-1][0], (y, points)
    i = min(max(1, bisect.bisect_left([p[0] for p in points], y)), len(points) - 1)
    (y0, c0), (y1, c1) = points[i - 1:i + 1]
    return math.exp(math.log(c0) + (y - y0) / (y1 - y0) * math.log(c1 / c0))


curves = {}
curve_sources = {}
for rate, label in [("8", "30fps"), ("0", "15fps")]:
    stages = [(phase, stage) for phase, stage in data["integration"]["phases"].items()
              if stage["complete"] and stage["label"].startswith(f"cal_r{rate}_t")
              and reg(stage, "3012") > 1]
    curves[label] = [(s["center_mean"], reg(s, "3012")) for _, s in stages]
    curve_sources[label] = [phase for phase, _ in stages]

# Declared output convention, not a measured gray-card/ISO anchor.
target_y = 118.0
# Timing model, inherited from the existing 15fps control table. Fine omitted.
frame_seconds = 0.066667
frame_lines = 704
coarse = 7
time_seconds = coarse * frame_seconds / frame_lines
reference_time = data["meter"]["shutter_seconds"]
reference_iso = data["meter"]["iso"]
assert data["meter"]["f_number"] == 2.8
rows = []
for phase, stage in data["gain"]["phases"].items():
    if stage["kind"] != "code":
        continue
    assert stage["complete"] and stage["locked_controls_held"]
    assert stage["measured_rows"] == 12
    assert reg(stage, "3012") == coarse
    assert reg(stage, "300a") == frame_lines
    assert reg(stage, "300c") == 843
    assert reg(stage, "3014") == 633
    assert reg(stage, "3032") == 256
    codes = [reg(stage, key) for key in ["3056", "3058", "305a", "305c"]]
    assert len(set(codes)) == 1
    y = stage["center_mean"]
    row = {"phase": phase, "four_channel_code": f"0x{codes[0]:04X}",
           "core_302a": reg(stage, "302a"), "center_y": y,
           "coarse_only_time_seconds": time_seconds}
    for label, points in curves.items():
        relative_dose = inverse_curve(points, y) / inverse_curve(points, target_y)
        row[f"conditional_ei_{label}_curve"] = reference_iso * reference_time / time_seconds * relative_dose
    # Deliberately different transfer assumption to expose model sensitivity.
    # Applying sRGB to mean Y is only a diagnostic proxy, not pixelwise radiometry.
    u = y / 255
    linear_proxy = u / 12.92 if u <= 0.04045 else ((u + 0.055) / 1.055) ** 2.4
    row["srgb_mean_y_proxy_ei"] = reference_iso * reference_time / time_seconds * linear_proxy / 0.18
    rows.append(row)

result = {
    "status": "conditional_reference_EI_model_estimates_NOT_validated_ISO",
    "sensor": 1,
    "method": "EI=100*(0.008/t7)*C(Ymeasured)/C(118); C=inverse integration-response curve",
    "time_model_seconds": time_seconds,
    "output_target_y": target_y,
    "curve_points_y_coarse": curves,
    "curve_source_phases": curve_sources,
    "assumptions": [
        "Nominal f/2.8 is applicable; external meter-02 matches illumination and ROI of gain run.",
        "15fps cadence/frame-lines gives row period; fine integration and timing offsets omitted.",
        "Processed Y transfer remains invariant across earlier integration and later gain runs, despite different illumination and channel gains.",
        "Y=118 is a declared middle-output convention; no known-reflectance gray target was measured.",
        "Piecewise log-coarse interpolation represents the unsampled response between measured points.",
    ],
    "limitations": [
        "30fps/15fps estimates are alternate models, not confidence limits or independently validated ISO measurements.",
        "No extrapolation beyond observed code points; maximum usable sensor ISO remains unknown.",
        "Y statistics are medians of frame center means; mean-Y inversion is not pixelwise inverse radiometry.",
        "Gain baseline returns differ from the target 0220 stage: evidence of drift/settling not removed by code hold.",
        "A same-batch fixed-gain integration sweep is absent; response-curve transfer is an unverified assumption.",
        "sRGB proxy is intentionally included as model sensitivity, not a calibrated competing ISO result.",
    ],
    "input_sha256": {str(path.relative_to(BASE)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in INPUTS.values()},
    "rows": rows,
}
(OUT / "conditional-ei.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
with (OUT / "conditional-ei.csv").open("w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
lines = [
    "# 现有数据的条件性参考 EI 初估",
    "",
    "这是离线模型拟合结果，不是已验证ISO或硬件ISO上下限。无需接入主程序即可计算。",
    "",
    "计算：t7=7×66.667ms/704=0.662882ms；EI=100×(8ms/t7)×C(Y)/C(118)。",
    "C通过已有积分扫描在Y—log(coarse)之间分段插值求逆；不做区间外推。",
    "两列来自不同帧率的旧响应曲线，列间差异不是误差条。",
    "",
    "|统一四通道代码|实读302A|中心Y|30fps曲线推算EI|15fps曲线推算EI|sRGB均值代理EI|",
    "|---|---:|---:|---:|---:|---:|",
]
for row in rows:
    lines.append(f"|{row['four_channel_code']}|{row['core_302a']}|{row['center_y']:.3f}|"
                 f"{row['conditional_ei_30fps_curve']:.1f}|{row['conditional_ei_15fps_curve']:.1f}|"
                 f"{row['srgb_mean_y_proxy_ei']:.1f}|")
lines += ["", "## 前提", ""] + ["- " + item for item in result["assumptions"]]
lines += ["", "## 限制", ""] + ["- " + item for item in result["limitations"]]
lines += ["", "原始数据只读；没有新增硬件采集、主程序或Vision运行时变更。",
          "计算脚本calculate.py可重复运行；JSON包含输入SHA-256和全部响应点。", ""]
(OUT / "RESULTS.md").write_text("\n".join(lines))
print("Six observed gain points calculated; no response-curve extrapolation.")
for row in rows:
    print(row["four_channel_code"], round(row["conditional_ei_30fps_curve"], 1),
          round(row["conditional_ei_15fps_curve"], 1), round(row["srgb_mean_y_proxy_ei"], 1))
