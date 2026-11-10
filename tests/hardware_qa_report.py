#!/usr/bin/env python3
from io import BytesIO, StringIO
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from hardware_qa import EXPECTED_SECTIONS, collect


def verification(rows):
    payload = ["F3QA1 test"]
    for section in sorted(EXPECTED_SECTIONS):
        payload.append(f"SECTION {section}")
        if section == "camera_color":
            payload.extend(rows)
    payload.append("DONE")
    report = collect(BytesIO(("\n".join(payload) + "\n").encode()), "test", Path("."), False, StringIO())
    return report["camera_verification"]


def main():
    manual = [
        "CAM_AUTO sensor=1 case=manual ae=0 ae_result=00000000 awb=1 awb_result=00000000",
        "COLOR sensor=1 case=exposure_low y_p50=50",
        "COLOR sensor=1 case=base y_p50=90",
        "COLOR sensor=1 case=exposure_high y_p50=150",
    ]
    hold = [
        "CAM_CALL sensor=1 case=manual_hold_set operation=SetExposure result=00000000",
        "COLOR sensor=1 case=manual_hold_a y_p50=90",
        "COLOR sensor=1 case=manual_hold_b y_p50=91",
        "STABILITY sensor=1 case=manual_hold_a ae_stable=1",
        "STABILITY sensor=1 case=manual_hold_b ae_stable=1",
        "CAM_AUTO sensor=1 case=manual_hold_after ae=0 ae_result=00000000 awb=1 awb_result=00000000",
    ]
    wb = [
        "CAM_CALL sensor=1 case=wb_auto_first_3 operation=SetAutoWhiteBalance result=00000000",
        "CAM_CALL sensor=1 case=wb_auto_first_3 operation=SetWhiteBalance result=00000000",
        "CAM_AUTO sensor=1 case=wb_auto_first_3 ae=0 ae_result=00000000 awb=1 awb_result=00000000",
    ]
    checks = verification(manual + hold + wb)
    assert checks["rear1"]["hardware_ae"]["status"] == "verified_off"
    assert checks["rear1"]["exposure_step_response"]["status"] == "observed"
    assert checks["rear1"]["fixed_step_output"]["status"] == "stable_in_sample"
    assert checks["rear1"]["physical_exposure_lock"]["status"] == "not_measurable"
    assert checks["rear1"]["fixed_white_balance"]["status"] == "failed_in_tested_cases"
    assert checks["rear2"]["hardware_ae"]["status"] == "inconclusive"

    drift = [row.replace("y_p50=91", "y_p50=105") for row in hold]
    awb_off = [row.replace("awb=1", "awb=0") for row in wb]
    checks = verification(manual + drift + awb_off)["rear1"]
    assert checks["fixed_step_output"]["status"] == "drift_observed"
    assert checks["fixed_white_balance"]["status"] == "controls_confirmed_color_unverified"

    checks = verification(manual + hold[:-1] + wb[:1] + awb_off[-1:])["rear1"]
    assert checks["fixed_step_output"]["status"] == "inconclusive"
    assert checks["fixed_white_balance"]["status"] == "inconclusive"
    print("PASS hardware QA separates output response, readback and unverified sensor settings")


if __name__ == "__main__":
    main()
