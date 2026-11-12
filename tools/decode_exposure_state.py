#!/usr/bin/env python3
"""Decode/verify existing FilmProbe CSV evidence. Never connects to a device."""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
from pathlib import Path

import exposure_register_contract as contract


def read_csv(path):
    with path.open(newline="", encoding="ascii") as file:
        reader = csv.DictReader(file)
        rows = list(reader)
        if not reader.fieldnames or any(None in r or any(v is None for v in r.values()) for r in rows):
            raise ValueError(f"malformed/truncated CSV: {path.name}")
        return rows


def success(value):
    return not (int(value, 16) & 0x80000000)


def verify_stage(stage, reads, frames, frame_scope="calibration-legacy"):
    """Keep failures explicit. Missing, duplicate or drifting fields never pass."""
    errors = []
    kind = stage["kind"]
    if kind not in contract.KINDS or int(stage["write_mask"]) not in contract.KINDS[kind]:
        raise ValueError("unregistered stage kind/write mask")
    n, settle = int(stage["measured"]), int(stage["settle"])
    if frame_scope not in contract.FRAME_SCOPES:
        raise ValueError("unknown frame scope")
    frame_rule=contract.FRAME_SCOPES[frame_scope]
    if isinstance(frame_rule,dict):frame_rule=frame_rule.get(kind,frame_rule['default'])
    if (settle, n) != ((1, 3) if kind == "integration_long" else frame_rule):
        raise ValueError("unregistered frame scope")
    expected = set(range(settle, settle + n))
    selected_frames = [r for r in frames if r["measured"] == "1"]
    if len(selected_frames) != n or {int(r["frame"]) for r in selected_frames} != expected:
        errors.append("missing_or_duplicate_measured_frame")
    if any(r["ok"] != "1" or r["received"] != "1" or not success(r["result"])
           or int(r["result"], 16) == 0x09401BFE for r in selected_frames):
        errors.append("capture_not_completed")

    indexed = defaultdict(list)
    for row in reads:
        if row["when"] in ("before", "after") and int(row["frame"]) in expected:
            indexed[(row["space"], row["addr"].lower(), row["when"], int(row["frame"]))].append(row)
    values = {}
    for space, address in contract.REQUIRED:
        observed = []
        for when in ("before", "after"):
            for frame in sorted(expected):
                rows = indexed[(space, address, when, frame)]
                if len(rows) != 1 or not success(rows[0]["result"]):
                    errors.append(f"missing_duplicate_or_failed:{space}:{address}:{when}:{frame}")
                else:
                    observed.append(int(rows[0]["value"]))
        if len(observed) == n * 2 and len(set(observed)) == 1:
            values[address] = observed[0]
        elif observed:
            errors.append(f"not_constant_or_incomplete:{space}:{address}")
    locked = stage["locked"] == "1"
    if not locked:
        errors.append("not_declared_locked")
    if any(values.get(address) != 0 for address in contract.LOCKS):
        errors.append("ae_or_awb_not_verified_off")
    if locked:
        for address, name in contract.TARGETS.items():
            if not stage[name] or values.get(address) != int(stage[name]):
                errors.append(f"requested_control_mismatch:{address}")

    def group(addresses, divisor=None):
        return {channel: {"address": address, "raw": values.get(address),
                          **({"configured_factor": values[address] / divisor
                              if address in values else None} if divisor else {})}
                for channel, address in addresses.items()}

    core = group(contract.CORE_DIGITAL, 256)
    isp = group(contract.ISP_DIGITAL, 128)
    core_known = all(v["raw"] is not None for v in core.values())
    isp_known = all(v["raw"] is not None for v in isp.values())
    # This describes register configuration, never claims that all pipeline
    # stages are enabled. Even full multiplier readback is not enable proof.
    core_unity = all(v["raw"] == 256 for v in core.values()) if core_known else None
    isp_nonunity = any(v["raw"] != 128 for v in isp.values()) if isp_known else None
    return {
        "sensor": int(stage["sensor"]), "phase": int(stage["phase"]), "label": stage["label"], "kind": kind,
        "proof_status": "verified_locked_sample" if not errors else "rejected",
        "errors": errors, "expected_frames": n,
        "readbacks_per_register_expected": n * 2,
        "timing_raw": {a: values.get(a) for a in contract.TIMING},
        "ae_mode": values.get("a11d"), "awb_mode": values.get("a11f"),
        "composite_gain_raw": group(contract.COMPOSITE),
        "analog_global_code": values.get("3028"), "analog_channel_codes": group(contract.ANALOG),
        "analog_multiplier_calibrated": False,
        "core_digital": core, "isp_digital": isp,
        "core_digital_unity_configured": core_unity,
        "isp_digital_nonunity_configured": isp_nonunity,
        "pipeline_enable_state": "not_verified_by_this_contract",
        "no_end_to_end_digital_amplification_proven": False,
        "absolute_iso_calibrated": False,
    }


def decode(directory, phases=None, frame_scope="calibration-legacy"):
    directory = Path(directory).resolve()
    names = ("calibration_plan.csv", "reads.csv", "frames.csv")
    plan, reads, frames = [read_csv(directory / name) for name in names]
    keys = [(r["sensor"], r["phase"]) for r in plan]
    if len(set(keys)) != len(keys):
        raise ValueError("duplicate planned stage")
    if phases and not set(phases) <= {int(r["phase"]) for r in plan}:
        raise ValueError("requested phase is absent from the declared plan")
    read_groups, frame_groups = defaultdict(list), defaultdict(list)
    for row in reads:
        read_groups[(row["sensor"], row["phase"])].append(row)
    for row in frames:
        frame_groups[(row["sensor"], row["phase"])].append(row)
    stages = [verify_stage(stage, read_groups[(stage["sensor"], stage["phase"])],
                           frame_groups[(stage["sensor"], stage["phase"])],frame_scope)
              for stage in plan if not phases or int(stage["phase"]) in phases]
    if not stages:
        raise ValueError("no declared stages selected")
    return {"schema": "film3ds-exposure-state-decoder-1", "boundary": "offline_read_only_evidence_analysis",
            "source": str(directory), "all_selected_stages_verified": all(s["proof_status"] == "verified_locked_sample" for s in stages),
            "register_reference": contract.REFERENCE,
            "limitations": ["gain factors describe configuration, not pipeline enable/bypass proof",
                            "analogue gain codes are not converted with the contradicted legacy formula",
                            "this tool does not infer absolute ISO, optical shutter time or continuous-preview lock"],
            "input_sha256": {name: hashlib.sha256((directory / name).read_bytes()).hexdigest() for name in names},
            "stages": stages}


def write_report(report, prefix):
    prefix = Path(prefix)
    outputs = [Path(str(prefix) + suffix) for suffix in (".json", ".csv", ".md")]
    source = Path(report["source"])
    if any(p.resolve() in {source / name for name in report["input_sha256"]} for p in outputs):
        raise ValueError("output would overwrite source evidence")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    outputs[0].write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    flat = []
    lines = ["# 自动曝光状态解码", "", "只解码原始证据，不连接或写入设备。倍率是寄存器配置，不证明管线使能。", "",
             "|Phase|积分|核心数字Gr/R/B/Gb|ISP数字Gr/R/B/Gb|判定|", "|---:|---:|---|---|---|"]
    for stage in report["stages"]:
        core = [stage["core_digital"][ch]["configured_factor"] for ch in contract.CHANNELS]
        isp = [stage["isp_digital"][ch]["configured_factor"] for ch in contract.CHANNELS]
        fmt = lambda values: "/".join("缺失" if v is None else f"{v:.6g}×" for v in values)
        lines.append(f"|{stage['phase']}|{stage['timing_raw']['3012']}|{fmt(core)}|{fmt(isp)}|{stage['proof_status']}|")
        flat.append({"sensor": stage["sensor"], "phase": stage["phase"], "coarse": stage["timing_raw"]["3012"],
                     "core_digital": fmt(core), "isp_digital": fmt(isp), "proof_status": stage["proof_status"],
                     "errors": ";".join(stage["errors"])})
    lines += ["", "模拟增益只输出实际代码，不套用已与实测冲突的倍率公式。",
              "AE/AWB、完整通道、逐帧前后读回均需满足合同；缺失、重复、失败或漂移均拒绝。",
              "不能把核心数字1×写成整个ISP无数字放大，也不能把配置倍率当成使能证明。", ""]
    outputs[2].write_text("\n".join(lines))
    with outputs[1].open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(flat[0]))
        writer.writeheader()
        writer.writerows(flat)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--phase", type=int, action="append")
    parser.add_argument("--out", type=Path, required=True, help="output prefix for JSON/CSV/Markdown")
    args = parser.parse_args()
    report = decode(args.directory, args.phase)
    write_report(report, args.out)
    print(f"{len(report['stages'])} stages; all_verified={report['all_selected_stages_verified']}; output={args.out}")
    return 0 if report["all_selected_stages_verified"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
