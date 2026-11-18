#!/usr/bin/env python3
"""Deploy Film3DS via NetLoader and collect a one-shot hardware report."""
import argparse
import ipaddress
import json
import secrets
import socket
import subprocess
from pathlib import Path

PORT = 17491
VGA_YUYV_BYTES = 640 * 480 * 2
EXPECTED_SECTIONS = {
    "system", "camera_timing", "camera_color", "native_rear1", "native_rear2",
    "native_front", "cpu_lut_and_gpu_tile", "gpu_full_resize", "gpu_three_layers",
}
SENSORS = {"1": "rear1", "2": "rear2", "3": "front"}
EXPOSURE_CASES = {"exposure_low": "-5", "base": "0", "exposure_high": "5"}
MANUAL_HOLD_CASES = {"manual_hold_a", "manual_hold_b"}


def read_line(stream):
    line = stream.readline(1025)
    if not line or len(line) > 1024 or not line.endswith(b"\n"):
        raise ValueError("report stopped before a complete line")
    return line.decode("ascii").rstrip("\n")


def fields(line):
    return dict(part.split("=", 1) for part in line.split()[1:])


def summarize_white_balance(rows):
    cases = {}
    for row in rows:
        name = row.get("case", "")
        if name.startswith(("wb_mode_first_", "wb_auto_first_", "wb_nobase_")):
            case = cases.setdefault(name, {"calls": {}})
            if row["type"] == "CAM_CALL":
                case["calls"][row["operation"]] = row["result"]
            elif row["type"] == "CAM_AUTO":
                case["awb_readback"] = row["awb"] if row["awb_result"] == "00000000" else None
            elif row["type"] == "STABILITY":
                case["sample_stability"] = row
    for name, case in cases.items():
        calls = case["calls"]
        awb = case.get("awb_readback")
        setter = "SetWhiteBalanceWithoutBaseUp" if name.startswith("wb_nobase_") else "SetWhiteBalance"
        if any(result != "00000000" for result in calls.values()) or awb == "1":
            case["status"] = "failed"
        elif {"SetAutoWhiteBalance", setter} <= calls.keys() and awb == "0":
            case["status"] = "controls_confirmed_color_unverified"
        else:
            case["status"] = "inconclusive"
    outcomes = {case["status"] for case in cases.values()}
    if "controls_confirmed_color_unverified" in outcomes:
        status = "controls_confirmed_color_unverified"
    elif outcomes == {"failed"}:
        status = "failed_in_tested_cases"
    else:
        status = "inconclusive"
    return {"status": status, "cases": cases}


def summarize_camera_controls(report):
    rows = report["sections"]["camera_color"]
    verification = {}
    for sensor, name in SENSORS.items():
        sensor_rows = [row for row in rows if row.get("sensor") == sensor]
        ae_reads = {row["case"]: row["ae"] if row["ae_result"] == "00000000" else None
                    for row in sensor_rows if row.get("type") == "CAM_AUTO"
                    and row.get("case") in {"exposure_sweep_before", "manual", "manual_hold_after"}}
        if "1" in ae_reads.values():
            ae_status = "reported_on"
        elif ae_reads.get("manual") == "0" and all(value == "0" for value in ae_reads.values()):
            ae_status = "verified_off"
        else:
            ae_status = "inconclusive"

        luma = {EXPOSURE_CASES[row["case"]]: int(row["y_p50"]) for row in sensor_rows
                if row.get("type") == "COLOR" and row.get("case") in EXPOSURE_CASES}
        response = "inconclusive"
        if ae_status == "verified_off" and len(luma) == len(EXPOSURE_CASES):
            response = "observed" if luma["-5"] < luma["0"] < luma["5"] else "not_observed"

        hold = [row for row in sensor_rows if row.get("type") == "STABILITY"
                and row.get("case") in MANUAL_HOLD_CASES]
        medians = {row["case"]: int(row["y_p50"]) for row in sensor_rows
                   if row.get("type") == "COLOR" and row.get("case") in MANUAL_HOLD_CASES}
        hold_set = any(row.get("type") == "CAM_CALL" and row.get("case") == "manual_hold_set"
                       and row["operation"] == "SetExposure" and row["result"] == "00000000"
                       for row in sensor_rows)
        hold_status = "inconclusive"
        if (hold_set and ae_status == "verified_off" and ae_reads.get("manual_hold_after") == "0"
                and {row["case"] for row in hold} == MANUAL_HOLD_CASES
                and medians.keys() == MANUAL_HOLD_CASES):
            within_bursts = all(row["ae_stable"] == "1" for row in hold)
            stable = within_bursts and max(medians.values()) - min(medians.values()) <= 2
            hold_status = "stable_in_sample" if stable else "drift_observed"

        verification[name] = {
            "hardware_ae": {"status": ae_status, "readbacks": ae_reads},
            "exposure_step_response": {"status": response, "y_median_by_step": luma},
            "fixed_step_output": {"status": hold_status, "last_y_medians": medians, "bursts": hold},
            "physical_exposure_lock": {"status": "not_measurable", "missing": ["integration_time", "sensor_gain"]},
            "fixed_white_balance": summarize_white_balance(sensor_rows),
        }
    return verification


def collect(stream, token, out, frames, log):
    if read_line(stream) != f"F3QA1 {token}":
        raise ValueError("report run token mismatch")
    report = {"sections": {}, "frames": []}
    section = None
    seen_sensors = set()
    while True:
        line = read_line(stream)
        log.write(line + "\n")
        log.flush()
        if line == "DONE":
            if set(report["sections"]) != EXPECTED_SECTIONS:
                raise ValueError("incomplete report sections")
            report["camera_verification"] = summarize_camera_controls(report)
            return report
        if line.startswith("SECTION "):
            section = line.removeprefix("SECTION ")
            if section not in EXPECTED_SECTIONS or section in report["sections"]:
                raise ValueError(f"invalid or duplicate section: {section}")
            report["sections"][section] = []
            print(f"Receiving {section}", flush=True)
            continue
        if section is None:
            raise ValueError("report data arrived before the first section")
        if line.startswith("FRAME "):
            meta = fields(line)
            sensor = int(meta["sensor"])
            if (not frames or section != "camera_color" or sensor not in (1, 2, 3)
                    or sensor in seen_sensors or meta["format"] != "YUYV"
                    or int(meta["bytes"]) != VGA_YUYV_BYTES
                    or (int(meta["width"]), int(meta["height"])) != (640, 480)
                    or int(meta["matrix"]) not in range(4)):
                raise ValueError("unexpected frame")
            payload = stream.read(VGA_YUYV_BYTES)
            if len(payload) != VGA_YUYV_BYTES or stream.read(1) != b"\n":
                raise ValueError(f"incomplete frame for sensor {sensor}")
            name = f"sensor-{sensor}.yuyv"
            (out / name).write_bytes(payload)
            report["frames"].append({"file": name, **meta})
            seen_sensors.add(sensor)
        elif line:
            kind = line.split(maxsplit=1)[0]
            structured = ("SYSTEM", "CAMERA", "CONTROL", "COLOR", "ZOOM", "ZOOM_RESTORE",
                          "CAM_CALL", "CAM_AUTO", "STABILITY", "SW_METER")
            row = {"type": kind, **fields(line)} if kind in structured else {"text": line}
            report["sections"][section].append(row)


def deploy_and_collect(args):
    target = str(ipaddress.IPv4Address(args.ip))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect((target, PORT))
        local = route.getsockname()[0]
    token = secrets.token_hex(16)
    args.out.mkdir(parents=True, exist_ok=False)
    command = [str(args.link), "-a", target, str(args.app), f"f3qa={token}"]
    if args.frames:
        command.append("f3qa-frames")
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((local, PORT))
        listener.listen(1)
        listener.settimeout(20)
        print(f"Return channel ready at {local}:{PORT}. Uploading to {target}.", flush=True)
        with (args.out / "upload.log").open("wb") as upload:
            subprocess.run(command, stdout=upload, stderr=subprocess.STDOUT, timeout=180, check=True)
        with (args.out / "report.log").open("w", encoding="ascii") as log:
            connection, peer = listener.accept()
            with connection:
                if peer[0] != target:
                    raise ValueError(f"unexpected report peer {peer[0]}")
                connection.settimeout(90)
                with connection.makefile("rb") as stream:
                    report = collect(stream, token, args.out, args.frames, log)
                with (args.out / "report.json").open("w") as file:
                    json.dump(report, file, indent=2)
                    file.write("\n")
                connection.sendall(b"K")
    print(f"Complete report at {args.out / 'report.json'}")
    for sensor, checks in report["camera_verification"].items():
        print(f"{sensor}: " + ", ".join(f"{name}={check['status']}" for name, check in checks.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ip", nargs="?", help="IPv4 address shown by the Homebrew Launcher NetLoader")
    parser.add_argument("app", nargs="?", type=Path)
    parser.add_argument("--link", type=Path, default="3dslink")
    parser.add_argument("--out", type=Path, help="new report directory")
    parser.add_argument("--frames", action="store_true", help="also return one baseline camera frame per lens")
    parser.add_argument("--summarize", type=Path, help="evaluate an existing report without contacting a 3DS")
    args = parser.parse_args()
    try:
        if args.summarize:
            print(json.dumps(summarize_camera_controls(json.loads(args.summarize.read_text())), indent=2))
        elif args.ip and args.app and args.out:
            deploy_and_collect(args)
        else:
            parser.error("provide IP, APP and --out, or use --summarize")
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        hint = f"\nSee {args.out} for upload or partial report logs." if args.out else ""
        parser.exit(1, f"{error}{hint}\n")


if __name__ == "__main__":
    main()
