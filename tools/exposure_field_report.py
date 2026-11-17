#!/usr/bin/env python3
"""Verify SD field evidence and frozen predictions; never operates the device."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image
from decode_exposure_state import decode, read_csv, write_report
from exposure_session import manifest_records, meter_rgb, yuyv_rgb


def analyse(directory, output, native_fixture=False):
    directory, output = Path(directory), Path(output)
    identity = manifest_records(directory, "FILMPROBE")
    if not native_fixture and (len(identity) != 1 or not identity[0].get("revision", "").startswith("exposure-field-")):
        raise ValueError("missing device build identity; native fixtures require explicit test mode")
    declaration = manifest_records(directory, "FIELD_CONTRACT")
    if len(declaration) != 1 or declaration[0] != dict(source="exposure-field", version="1", max_stages="8",
            fps="5", coarse_min="1", coarse_max="1585", settle_gain="8", settle_coarse="2",
            measured="4", references="post_prediction", retry="none"):
        raise ValueError("unknown or widened field contract")
    proof = decode(directory, frame_scope="exposure-field/v1")
    stages = proof["stages"]
    if not 2 <= len(stages) <= 8 or stages[0]["label"] != "field_seed":
        raise ValueError("invalid field stage count/source")
    labels = [s["label"] for s in stages]
    expected = ["field_seed", "field_normal_base"]
    normal_steps = sum(name.startswith("field_normal_") and name[-1:].isdigit() for name in labels)
    if normal_steps > 3:
        raise ValueError("normal decision budget exceeded")
    expected += [f"field_normal_{i}" for i in range(normal_steps)]
    expected += [name for name in ("field_lower", "field_upper", "field_normal_check") if name in labels]
    if labels != expected:
        raise ValueError("unregistered field stage order")
    by_phase = {s["phase"]: s for s in stages}
    locked = stages[1:]
    end = manifest_records(directory, "FIELD_END")
    valid = len(end) == 1 and end[0]["complete"] == "1" and end[0]["restored"] == "1"
    if valid and (labels[-1] != "field_normal_check" or int(end[0]["stages"]) != len(stages)):
        raise ValueError("complete field run lacks normal recheck or stage proof")
    valid &= all(s["proof_status"] == "verified_locked_sample" for s in locked)
    if valid:
        if any(not s["core_digital_unity_configured"] or s["timing_raw"]["300a"] != 2114 for s in locked):
            raise ValueError("field digital/timing contract mismatch")
        for s in locked[1:]:
            first = locked[0]
            if s["analog_channel_codes"] != first["analog_channel_codes"] or s["isp_digital"] != first["isp_digital"]:
                raise ValueError("cross-stage analog/ISP drift")
            if any(s["timing_raw"][k] != first["timing_raw"][k] for k in first["timing_raw"] if k != "3012"):
                raise ValueError("cross-stage timing drift")
    output.mkdir(parents=True, exist_ok=True)
    write_report(proof, output / "controls")
    prior = dict(line.split("=", 1) for line in (directory / "model-before.txt").read_text().splitlines() if "=" in line)
    meters = {int(m["phase"]): m for m in manifest_records(directory, "AUTO_METER")}
    if any(float(m["span_ev"]) > .125 or m["complete"] != "1" for phase, m in meters.items() if phase in {s["phase"] for s in locked}):
        valid = False
    burst = read_csv(directory / "frame_burst.csv")
    if len(burst) != 24 or any(r["rate"] != "5" or r["received"] != "1" or int(r["wait_result"], 16) != 0 or int(r["receive_result"], 16) != 0 or r["buffer_error"] != "0" for r in burst):
        raise ValueError("missing/failed measured frame-cadence evidence")
    period = (int(burst[-1]["time_ms"]) - int(burst[0]["time_ms"])) / 23000
    if period <= 0:
        raise ValueError("invalid measured frame period")
    photos = []
    for row in read_csv(directory / "exposure.csv"):
        if row["measured"] != "1":
            continue
        phase = int(row["phase"])
        if phase not in by_phase or row["label"] != by_phase[phase]["label"] or row["coarse"] != str(by_phase[phase]["timing_raw"]["3012"]):
            raise ValueError("photo/parameter association mismatch")
        m = meters[phase]
        if m["complete"] != "1" or by_phase[phase]["proof_status"] != "verified_locked_sample":
            valid = False
            continue
        raw = directory / f"s1-p{phase:03d}-{row['label']}.yuyv"
        rgb = yuyv_rgb(raw, int(m["matrix"]))
        meter = meter_rgb(rgb)
        native_difference = abs(meter["linear_mean"] - float(m["last_linear_luma"]))
        if native_difference > 1e-7:
            raise ValueError("host/native RGB metering mismatch")
        dose = float(row["dose"])
        state = by_phase[phase]
        timing = state["timing_raw"]
        integration = (timing["3012"] + timing["3014"] / timing["300c"]) * period / timing["300a"]
        actual_dose = integration * state["analog_channel_codes"]["Gr"]["raw"] / 8 * state["isp_digital"]["Gr"]["configured_factor"]
        if not math.isclose(float(row["integration_est_s"]), integration, rel_tol=1e-8) or not math.isclose(dose, actual_dose, rel_tol=1e-8):
            raise ValueError("simulated integration/dose differs from raw hardware timing/gain")
        predicted = float(row["predicted_A_s"])
        computed = 2 ** (float(prior["a"]) + float(prior["b"]) * math.log2(dose)) if int(prior["labels"]) else math.nan
        if math.isfinite(computed) and not math.isclose(predicted, computed, rel_tol=1e-8):
            raise ValueError("prediction differs from immutable pre-reference model")
        jpeg = output / (raw.stem + ".jpg")
        Image.fromarray(np.rint(rgb * 255).astype(np.uint8)).save(jpeg, quality=95, subsampling=0)
        with Image.open(jpeg) as image:
            jpeg_meter = meter_rgb(np.asarray(image) / 255)
        jpeg_ev = math.log2(max(jpeg_meter["linear_mean"], 1e-12) / max(meter["linear_mean"], 1e-12))
        if meter["linear_mean"] >= .005 and abs(jpeg_ev) > .05:
            valid = False
        photos.append({**row, "native_meter_abs_difference": native_difference, "jpeg_error_ev": jpeg_ev,
                       "raw_sha256": hashlib.sha256(raw.read_bytes()).hexdigest(), "source_meter": meter})
    reference_path = directory / "reference-A.csv"
    reference = read_csv(reference_path) if reference_path.exists() else []
    if len(reference) > 1:
        raise ValueError("multiple references in a single frozen run")
    error = None
    if reference:
        normal = next(p for p in photos if p["slot"] == "1")
        ref = reference[0]
        if ref["meter"] != "A" or float(ref["iso"]) != 100 or float(ref["f_number"]) != 2.8:
            raise ValueError("unregistered reference settings")
        error = math.log2(float(normal["predicted_A_s"]) / float(ref["reference_s"]))
        if math.isfinite(error) and not math.isclose(error, float(ref["error_ev"]), abs_tol=1e-8):
            raise ValueError("independent reference error differs from device")
    result = {"source": "exposure-field/v1", "hardware_proof_verified": bool(valid and not native_fixture),
              "contract_proof_verified": bool(valid),
              "evidence_origin": "native_C_fault_injection_fixture" if native_fixture else "device_SD_capture",
              "photos": photos,
              "prior_model": prior, "reference": reference, "prediction_error_ev": error,
              "absolute_iso_calibrated": False, "no_model_refit_on_validation_sample": True}
    (output / "field-analysis.json").write_text(json.dumps(result, indent=2) + "\n")
    rows = ["# 離線曝光驗證", "", f"證據來源：{result['evidence_origin']}。",
            f"契約檢查通過：{bool(valid)}；實機證據：{result['hardware_proof_verified']}。", "",
            "|候選|積分|感測器模擬秒數|A快門預測秒數|預測EV100|外推|", "|---|---:|---:|---:|---:|---|"]
    for p in photos:
        rows.append(f"|{p['label']}|{p['coarse']}|{p['integration_est_s']}|{p['predicted_A_s']}|{p['predicted_ev100']}|{p['extrapolated']}|")
    rows += ["", f"與當輪A參考的預測誤差：{error} EV。",
             "模型於輸入參考前凍結；本輪標籤不回填本輪預測。",
             "感測器秒數、增益歸一化與初始單點斜率仍是待實測驗證的模型假設。",
             "內部亮度0.18達標不等於第三方曝光值匹配；JPEG一致性也不等於獨立物理校準。", ""]
    (output / "RESULTS.md").write_text("\n".join(rows))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--native-fixture", action="store_true", help="Test-only: never labels results as actual hardware evidence")
    args = parser.parse_args()
    result = analyse(args.directory, args.out, args.native_fixture)
    print(json.dumps({k: result[k] for k in ("hardware_proof_verified", "prediction_error_ev")}, indent=2))
    raise SystemExit(0 if result["contract_proof_verified"] else 2)
