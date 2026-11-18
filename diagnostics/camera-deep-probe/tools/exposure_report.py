#!/usr/bin/env python3
"""Analyze a FilmProbe SD run, preserving failures and incomplete experiments."""
import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path

PHYSICAL = ("0010", "0012", "0014", "300a", "300c", "3016", "3012", "3014", "3056", "3058", "305a", "305c",
            "3032", "3034", "3036", "3038", "32d4", "32d6", "32d8", "32da")
WRITE_OPS = {"WriteRegisterI2c", "WriteMcuVariableI2c"}


def success(row):
    # libctru Result succeeds when signed nonnegative, not only when exactly zero.
    return not (int(row["result"], 16) & 0x80000000)


def load_csv(directory, name, columns):
    with (directory / name).open(newline="", encoding="ascii") as file:
        reader = csv.DictReader(file)
        if not set(columns) <= set(reader.fieldnames or ()):
            raise ValueError(f"{name}: missing columns")
        rows = list(reader)
    for row in rows:
        if None in row or any(value is None for value in row.values()):
            raise ValueError(f"{name}: truncated/malformed CSV row")
    return rows


def phase_summary(reads, frames):
    measured = [f for f in frames if f["measured"] == "1" and f["ok"] == "1"
                and f["received"] == "1" and success(f)]
    after = [r for r in reads if r["when"] == "after"]
    valid_frames = {f["frame"] for f in measured}
    registers = {}
    for addr in PHYSICAL:
        rows = [r for r in after if r["space"] == "reg" and r["addr"] == addr
                and r["frame"] in valid_frames]
        values = [int(r["value"]) for r in rows if success(r)]
        full = len(rows) == 12 and len(values) == 12 and len({r["frame"] for r in rows}) == 12
        registers[addr] = {"complete": full, "values": values,
                           "constant": full and len(set(values)) == 1}
    full_frames = len(measured) == 12 and valid_frames == {str(i) for i in range(8, 20)}
    complete = full_frames and all(r["complete"] for r in registers.values())
    stable = complete and all(r["constant"] for r in registers.values())
    return {
        "label": (frames or reads or [{}])[0].get("label", "unknown"),
        "frames_complete": full_frames,
        "measured_frames": len(measured),
        "y_mean": statistics.median(float(f["y_mean"]) for f in measured) if measured else None,
        "y_span": max(float(f["y_mean"]) for f in measured) - min(float(f["y_mean"]) for f in measured)
        if measured else None,
        "physical_state": "constant_in_sample" if stable else "changed_in_sample" if complete else "inconclusive",
        "registers": registers,
    }


def direct_summary(calls, reads, phases):
    trials = []
    for phase in sorted({int(c["phase"]) for c in calls if c["operation"] in WRITE_OPS
                         and c["label"].startswith("direct_")}):
        writes = [c for c in calls if int(c["phase"]) == phase and c["operation"] in WRITE_OPS]
        current, restored = phases.get(str(phase), {}), phases.get(str(phase + 1), {})
        restore_calls = [c for c in calls if int(c["phase"]) == phase + 1
                         and c["operation"] in WRITE_OPS and c["label"] == "restore"]
        label = writes[0]["label"]
        expected = {"3012"} if label == "direct_integration_low" else {"a206"} if label == "direct_ae_target_low" else {"3056", "3058", "305a", "305c"}
        accepted = {c["addr"] for c in writes} == expected and all(success(c) for c in writes)

        def matched(operations, at, when, frame=None):
            return bool(operations) and all(any(
                int(r["phase"]) == at and r["when"] == when
                and r["space"] == ("mcu" if c["operation"] == "WriteMcuVariableI2c" else "reg")
                and r["addr"] == c["addr"] and success(r) and int(r["value"]) == int(c["value"])
                and (frame is None or r["frame"] == frame) for r in reads) for c in operations)

        immediate = accepted and matched(writes, phase, "immediate")
        retained = accepted and current.get("frames_complete", False) and all(
            matched(writes, phase, "after", str(frame)) for frame in range(8, 20))
        restore_ok = ({c["addr"] for c in restore_calls} == expected
                      and all(success(c) for c in restore_calls)
                      and matched(restore_calls, phase + 1, "immediate"))
        restore_retained = restore_ok and restored.get("frames_complete", False) and all(
            matched(restore_calls, phase + 1, "after", str(frame)) for frame in range(8, 20))
        baseline = phases.get(str(phase - 1), {})
        base_y, target_y, restore_y = (s.get("y_mean") for s in (baseline, current, restored))
        image = "inconclusive"
        controls = "inconclusive"
        # Require full physical snapshots, and unchanged non-target controls across A-B-A.
        if all(s.get("physical_state") == "constant_in_sample" for s in (baseline, current, restored)):
            others = set(PHYSICAL) - expected
            controls = "unchanged" if all(
                len({s["registers"][addr]["values"][0] for s in (baseline, current, restored)}) == 1
                for addr in others) else "other_controls_changed"
        if (all(s.get("frames_complete") for s in (baseline, current, restored))
                and all(y is not None for y in (base_y, target_y, restore_y))):
            tolerance = max(2.0, abs(base_y) * 0.05)
            reversible = abs(restore_y - base_y) <= tolerance
            response = target_y < base_y - tolerance  # all current trials lower exposure/gain/target
            image = "changed_and_returned" if response and reversible else "unexpected_direction" if target_y > base_y + tolerance else "no_clear_reversible_response"
        if not accepted:
            status = "write_failed_or_partial"
        elif not immediate:
            status = "accepted_readback_not_confirmed"
        elif not retained:
            status = "immediate_only_or_incomplete"
        elif not restore_retained:
            status = "restore_unconfirmed_or_incomplete"
        elif image == "changed_and_returned" and controls == "unchanged":
            status = "register_and_image_response_observed"
        else:
            status = "register_control_observed_image_inconclusive"
        trials.append({"phase": phase, "label": label, "status": status,
                       "api_accepted": accepted, "immediate_readback": immediate,
                       "retained_after_frames": retained, "restored_after_frames": restore_retained,
                       "image_response": image, "other_controls": controls,
                       "y_before_target_restore": [base_y, target_y, restore_y]})
    return trials


def summarize(directory):
    directory = Path(directory)
    calls = load_csv(directory, "calls.csv", ("sensor", "phase", "operation", "result", "addr", "value"))
    reads = load_csv(directory, "reads.csv", ("sensor", "phase", "frame", "when", "space", "addr", "value", "result"))
    frames = load_csv(directory, "frames.csv", ("sensor", "phase", "frame", "measured", "ok", "result", "received", "y_mean"))
    manifest = (directory / "manifest.txt").read_text(encoding="ascii")
    sensors = {}
    for sensor in ("1", "2", "3"):
        cs, rs, fs = ([r for r in rows if r["sensor"] == sensor] for rows in (calls, reads, frames))
        caps = defaultdict(list)
        for row in cs:
            caps[row["operation"]].append(row)
        capabilities = {}
        for name, rows in caps.items():
            ok = sum(success(r) for r in rows)
            capabilities[name] = {"status": "accepted" if ok == len(rows) else "rejected" if not ok else "mixed",
                                  "calls": len(rows), "successes": ok,
                                  "results": sorted({r["result"] for r in rows})}
        for space, name in (("reg", "ReadRegisterI2cExclusive"), ("mcu", "ReadMcuVariableI2cExclusive")):
            rows = [r for r in rs if r["space"] == space]
            capabilities[name] = {"status": "not_tested" if not rows else "accepted" if all(success(r) for r in rows)
                                  else "mixed" if any(success(r) for r in rows) else "rejected",
                                  "addresses": {addr: sorted({r["result"] for r in rows if r["addr"] == addr})
                                                for addr in sorted({r["addr"] for r in rows})}}
        phases = {str(phase): phase_summary([r for r in rs if int(r["phase"]) == phase],
                                           [f for f in fs if int(f["phase"]) == phase])
                  for phase in sorted({int(r["phase"]) for r in rs + fs})}
        ids = sorted({int(r["value"]) for r in rs if r["addr"] == "0000" and success(r)})
        exposure_steps = []
        for call in cs:
            if call["operation"] != "SetExposure":
                continue
            phase = int(call["phase"])
            current = phases.get(str(phase), {})
            before, after = phases.get(str(phase - 1), {}), phases.get(str(phase + 1), {})
            changed = []
            for addr in PHYSICAL:
                a = before.get("registers", {}).get(addr, {})
                b = current.get("registers", {}).get(addr, {})
                if a.get("constant") and b.get("constant") and a["values"][0] != b["values"][0]:
                    changed.append(addr)
            value = int(call["value"])
            if value >= 2**31:
                value -= 2**32
            exposure_steps.append({"phase": phase, "requested": value, "api_accepted": success(call),
                                   "physical_state": current.get("physical_state", "inconclusive"),
                                   "changed_registers_vs_previous_phase": changed,
                                   "y_before_target_after": [s.get("y_mean") for s in (before, current, after)]})
        sensors[sensor] = {"chip_ids": ids, "capabilities": capabilities, "phases": phases,
                           "exposure_steps": exposure_steps,
                           "direct_trials": direct_summary(cs, rs, phases)}
    workflow_complete = any(line.startswith("END complete=1 ") for line in manifest.splitlines())
    requests = [dict(part.split("=",1) for part in line.split()[1:])
                for line in manifest.splitlines() if line.startswith("REQUEST ")]
    request = requests[-1] if requests else {"only_sensor":"0","first_phase":"3","last_phase":"72"}
    only, first, last = (int(request[k]) for k in ("only_sensor","first_phase","last_phase"))
    if only not in (0,1,2,3) or not 3 <= first <= 58 or last not in (58,72) or first > last or (only == 0 and first != 3):
        raise ValueError("invalid diagnostic request scope")
    required = [str(only)] if only else ["1","2","3"]
    coverage = {}
    for sensor in required:
        phases = sensors[sensor]["phases"]
        missing = [phase for phase in range(first,last+1) if not phases.get(str(phase),{}).get("frames_complete")]
        incomplete_reads = [phase for phase in range(first,last+1)
                            if not all(phases.get(str(phase),{}).get("registers",{}).get(addr,{}).get("complete",False) for addr in PHYSICAL)]
        coverage[sensor] = {"first_phase":first,"last_phase":last,
                            "incomplete_frame_phases":missing,"incomplete_register_phases":incomplete_reads}
    measurement_records = [dict(part.split("=",1) for part in line.split()[1:])
                           for line in manifest.splitlines() if line.startswith("MEASUREMENTS ")]
    sensor_sampling_complete = all(any(line.startswith(f"SENSOR_END sensor={sensor} complete=1")
                                       for line in manifest.splitlines()) for sensor in required)
    sampling_complete = bool(sensor_sampling_complete and measurement_records
                             and measurement_records[-1].get("all_complete") == "1"
                             and int(measurement_records[-1].get("expected","0")) == len(required)*(last-first+1)
                             and all(not c["incomplete_frame_phases"] for c in coverage.values()))
    register_reads_complete = all(not c["incomplete_register_phases"] for c in coverage.values())
    return {"schema":3,"workflow_complete":workflow_complete,"sampling_complete":sampling_complete,
            "register_reads_complete":register_reads_complete,"request":request,"coverage":coverage,
            "run_complete":workflow_complete and sampling_complete and register_reads_complete,
            "manifest": manifest.splitlines(), "sensors": sensors,
            "limits": ["API acceptance alone never proves exposure control.",
                       "Fixed scene/illumination is an operator condition, not established by this probe.",
                       "Register reads bracket reception; they are not atomic frame metadata.",
                       "No ISO calibration, aperture value or shutter seconds inferred.",
                       "MCU writes cover AE target only. Privileged services, RAW capture and arbitrary addresses are not tested."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        report = summarize(args.directory)
        path = args.directory / "analysis.json"
        path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Run complete: {report['run_complete']}; details: {path}")
        for sensor, result in report["sensors"].items():
            print(f"Sensor {sensor}: IDs {result['chip_ids']}")
            for trial in result["direct_trials"]:
                print(f"  phase {trial['phase']} {trial['label']}: {trial['status']}")
        return 0 if report["run_complete"] else 2
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Cannot analyze complete CSV data: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
