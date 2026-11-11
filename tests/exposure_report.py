#!/usr/bin/env python3
"""Failure-oriented tests: acceptance, delayed overwrite and image effect differ."""
import importlib.util
from pathlib import Path
import tempfile

spec = importlib.util.spec_from_file_location("probe_report", Path(__file__).resolve().parents[1] / "tools/exposure_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def fixture(mode):
    calls, reads, frames = [], [], []
    for phase, label, y in ((1, "direct_baseline", 100), (2, "direct_integration_low", 50), (3, "restore", 100)):
        for frame in range(8, 20):
            frames.append(dict(sensor="1", phase=str(phase), label=label, frame=str(frame),
                               measured="1", ok="1", received="1", result="00000000", y_mean=str(y)))
            for addr in report.PHYSICAL:
                v = 50 if phase == 2 and addr == "3012" else 100
                if mode == "ignored" and phase == 2:
                    v = 100
                if mode == "overwrite" and phase == 2 and frame > 8:
                    v = 100
                if mode == "other_gain" and phase == 2 and addr == "3032":
                    v = 200
                if mode == "restore_drift" and phase == 3 and addr == "3012":
                    v = 110
                reads.append(dict(sensor="1", phase=str(phase), label=label, frame=str(frame),
                                  when="after", space="reg", addr=addr, value=str(v), result="00000000"))
        if phase in (2, 3):
            value = "50" if phase == 2 else "100"
            calls.append(dict(sensor="1", phase=str(phase), label=label, operation="WriteRegisterI2c",
                              addr="3012", value=value, result="00000000"))
            reads.append(dict(sensor="1", phase=str(phase), label=label, frame="-1", when="immediate",
                              space="reg", addr="3012", value="100" if mode == "ignored" else value,
                              result="00000000"))
    if mode == "missing_frame":
        frames.pop()
    if mode == "failed_read":
        reads[0]["result"] = "d900182f"
    if mode == "write_denied":
        calls[0]["result"] = "d900182f"
    if mode == "no_image":
        for f in frames:
            f["y_mean"] = "100"
    if mode == "unexpected_direction":
        for f in frames:
            if f["phase"] == "2":
                f["y_mean"] = "150"
    if mode == "no_physical_baseline":
        reads = [r for r in reads if r["phase"] != "1"]
    phases = {str(p): report.phase_summary([r for r in reads if r["phase"] == str(p)],
                                          [r for r in frames if r["phase"] == str(p)]) for p in (1, 2, 3)}
    return report.direct_summary(calls, reads, phases)[0]


def main():
    assert fixture("normal")["status"] == "register_and_image_response_observed"
    assert fixture("ignored")["status"] == "accepted_readback_not_confirmed"
    assert fixture("overwrite")["status"] == "immediate_only_or_incomplete"
    assert fixture("missing_frame")["status"] == "restore_unconfirmed_or_incomplete"
    assert fixture("restore_drift")["status"] == "restore_unconfirmed_or_incomplete"
    assert fixture("write_denied")["status"] == "write_failed_or_partial"
    for mode in ("no_image", "other_gain", "failed_read", "no_physical_baseline", "unexpected_direction"):
        assert fixture(mode)["status"] == "register_control_observed_image_inconclusive", mode
    assert report.success({"result": "00000001"})
    assert not report.success({"result": "80000000"})
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / "calls.csv").write_text("sensor,phase,operation,result,addr,value\n1,0,Read,0,0000\n")
        try:
            report.load_csv(d, "calls.csv", ("sensor", "phase", "operation", "result", "addr", "value"))
        except ValueError:
            pass
        else:
            raise AssertionError("truncated row accepted")
    print("PASS exposure report: ignored writes, overwrite, drift, failed reads, partial frames and confounding gain")


if __name__ == "__main__":
    main()
