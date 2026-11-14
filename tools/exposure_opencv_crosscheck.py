#!/usr/bin/env python3
"""Independent OpenCV inverse-response check on an existing static burst.

No device command, image enhancement, response extrapolation or parameter search.
The held-out exposure never trains the response. Channel failures stay failures.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import cv2
import numpy as np


def check(directory, output):
    directory, output = Path(directory), Path(output)
    analysis = json.loads((directory / "session-analysis.json").read_text())
    if analysis["contract"] != "exposure-auto/v1" or not analysis["hardware_evidence_verified"]:
        raise ValueError("this registered comparison requires the verified v1 static sequence")
    images = {i["label"]: i for i in analysis["images"] if i["status"] == "verified"}
    stages = {s["label"]: s for s in analysis["decoded_controls"]["stages"]}
    training = ["auto_gain_base", "auto_normal_base", "auto_normal_1", "auto_normal_2"]
    holdout = "auto_normal_0"
    controls = stages[training[0]]
    for name in training + [holdout]:
        state = stages[name]
        if state["composite_gain_raw"] != controls["composite_gain_raw"] or state["isp_digital"] != controls["isp_digital"]:
            raise ValueError("response sequence changed color/gain state")
    # Predeclared static center-frame strip, shared across every image. No ROI
    # search to improve a failing channel. Gain-test coverage is checked below.
    roi = (304, 196, 368, 280)
    x0, y0, x1, y1 = roi
    def load(name):
        image = cv2.imread(str(directory / images[name]["jpeg"]), cv2.IMREAD_COLOR)
        if image is None or image.shape != (480, 640, 3):
            raise ValueError("missing/wrong-size verified JPEG")
        return image[y0:y1, x0:x1].copy()
    train = [load(n) for n in training]
    times = np.array([stages[n]["timing_raw"]["3012"] for n in training], dtype=np.float32)
    response = cv2.createCalibrateDebevec(100, 10.0, False).process(train, times=times)
    if response.shape != (256, 1, 3) or not np.all(np.isfinite(response)) or np.any(response <= 0):
        raise ValueError("invalid OpenCV inverse response")
    response = response[:, 0, :]
    def response_mean(image):
        return np.array([np.mean(response[image[:, :, c], c]) for c in range(3)])
    baseline = response_mean(train[0])
    held = load(holdout)
    ratio = response_mean(held) / baseline * times[0] / stages[holdout]["timing_raw"]["3012"]
    errors = np.log2(ratio)
    lo = np.min(np.stack(train), axis=(0, 1, 2))
    hi = np.max(np.stack(train), axis=(0, 1, 2))
    channel_rows = [{"channel": c, "holdout_error_ev": float(e), "passed_0125ev": bool(abs(e) <= .125),
                     "trained_code_interval": [int(a), int(b)]}
                    for c, e, a, b in zip(("B", "G", "R"), errors, lo, hi)]
    gain_rows = []
    for name in images:
        if not name.startswith(("auto_gain_", "auto_digital_")) or name == "auto_gain_base":
            continue
        image = load(name)
        effective = response_mean(image) / baseline * times[0] / stages[name]["timing_raw"]["3012"]
        for c, channel in enumerate(("B", "G", "R")):
            inside = np.mean((image[:, :, c] >= lo[c]) & (image[:, :, c] <= hi[c]))
            accepted = inside >= .95 and abs(errors[c]) <= .125
            gain_rows.append({"label": name, "channel": channel, "in_domain_fraction": float(inside),
                              "effective_gain": float(effective[c]) if accepted else None,
                              "status": "crosschecked" if accepted else "rejected_domain_or_holdout"})
    result = {"tool": "OpenCV CalibrateDebevec", "version": cv2.__version__, "training": training,
              "holdout": holdout, "roi_xyxy": roi, "samples": 100, "lambda": 10, "random": False,
              "source_analysis_sha256": hashlib.sha256((directory / "session-analysis.json").read_bytes()).hexdigest(),
              "channels": channel_rows, "gain_rows": gain_rows,
              "all_channels_passed": all(c["passed_0125ev"] for c in channel_rows),
              "absolute_ev100_calibrated": False,
              "limitations": ["same scene/staticness and stable processing required",
                              "inverse response has relative scale, not an independent scene EV100",
                              "post-run external meter does not prove natural light remained constant"]}
    output.mkdir(parents=True, exist_ok=True)
    np.save(output / "opencv-response.npy", response)
    (output / "opencv-crosscheck.json").write_text(json.dumps(result, indent=2) + "\n")
    rows = ["# OpenCV 第三方響應交叉驗證", "", "未參與擬合的曝光用作留出驗證；不把JPEG轉換的一致性當獨立曝光標準。", "",
            "|通道|留出誤差EV|±0.125EV通過|", "|---|---:|---|"]
    rows += [f"|{c['channel']}|{c['holdout_error_ev']:.5f}|{c['passed_0125ev']}|" for c in channel_rows]
    rows += ["", "域外或留出驗證失敗的通道不輸出有效增益。此工具辨識相對響應，EV100尺度仍需外部曝光標籤。", "",
             "[OpenCV 官方算法說明](https://docs.opencv.org/4.12.0/d2/df0/tutorial_py_hdr.html)", ""]
    (output / "RESULTS.md").write_text("\n".join(rows))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    result = check(args.directory, args.out)
    print(json.dumps({k: result[k] for k in ("channels", "all_channels_passed")}, indent=2))
