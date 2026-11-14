#!/usr/bin/env python3
"""One explicitly requested sweep deployment; bounded read-only recovery wait."""
import argparse
import contextlib
import hashlib
import http.client
import json
from pathlib import Path
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "build/worktrees/camera-deep-probe"
sys.path.insert(0, str(PROBE / "tools"))
from run_deep_probe import deploy
from retrieve_probe_ram import retrieve, parse_index
from exposure_sweep_contract import CONTRACT_MATRIX, SOURCE, CHECK_SOURCE, REFRESH_SOURCE, MANUAL_SOURCE, UPDATE_SOURCE, NO_RECEIVE_SOURCE, WRITER_WINDOW_SOURCE, WRITER_OBSERVE_SOURCE, HG_SOURCE, CCM_SOURCE, CCM_BYTES_SOURCE, CCM_BOOT_SOURCE, GAIN_MATRIX_SOURCE, EXPOSURE_REBUILD_SOURCE, EXPOSURE_DENSE_SOURCE


def run(ip, output, before, check=False, awb_refresh=False, post_refresh_manual=False, isp_update=False,no_receive=False,writer_window=False,hg_isolation=False,ccm_normalization=False,gain_matrix=False,exposure_rebuild=False,exposure_dense=False):
    """Keep network/file detail off the console, including on failure."""
    if sum((check,awb_refresh,post_refresh_manual,isp_update,no_receive,writer_window,hg_isolation,ccm_normalization,gain_matrix,exposure_rebuild,exposure_dense))>1:raise ValueError("choose one registered diagnostic")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    console = sys.stdout
    def status(phase, **fields):
        record = dict(phase=phase, updated_unix=time.time(), **fields)
        temporary = output / "session-status.json.tmp"
        temporary.write_text(json.dumps(record)+"\n")
        temporary.replace(output / "session-status.json")
        print(json.dumps(record), file=console, flush=True)
    with (output / "session.log").open("x", buffering=1) as log:
        with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            try:
                status("deploying", diagnostic=check or awb_refresh or post_refresh_manual or isp_update or no_receive or writer_window or hg_isolation or ccm_normalization or gain_matrix or exposure_rebuild or exposure_dense)
                _run(ip, output, before, check, status, awb_refresh, post_refresh_manual,isp_update,no_receive,writer_window,hg_isolation,ccm_normalization,gain_matrix,exposure_rebuild,exposure_dense)
            finally:
                error = sys.exc_info()[1]
                if error is not None:
                    traceback.print_exc()
                    status("failed", error_type=type(error).__name__, detail_log="session.log",
                           automatic_redeployment=False)


def _run(ip, output, before, check, status, awb_refresh=False, post_refresh_manual=False,isp_update=False,no_receive=False,writer_window=False,hg_isolation=False,ccm_normalization=False,gain_matrix=False,exposure_rebuild=False,exposure_dense=False):
    source_name = EXPOSURE_DENSE_SOURCE if exposure_dense else EXPOSURE_REBUILD_SOURCE if exposure_rebuild else GAIN_MATRIX_SOURCE if gain_matrix else CCM_BOOT_SOURCE if ccm_normalization else HG_SOURCE if hg_isolation else WRITER_OBSERVE_SOURCE if writer_window else NO_RECEIVE_SOURCE if no_receive else UPDATE_SOURCE if isp_update else MANUAL_SOURCE if post_refresh_manual else REFRESH_SOURCE if awb_refresh else CHECK_SOURCE if check else SOURCE
    session_name = "camera-exposure-dense-session/v1" if exposure_dense else "camera-exposure-rebuild-session/v2" if exposure_rebuild else "camera-gain-matrix-session/v4" if gain_matrix else "camera-ccm-normalization-session/v3" if ccm_normalization else "camera-hg-isolation-session/v1" if hg_isolation else "camera-writer-trace-session/v2" if writer_window else "no-receive-frame-update-session/v1" if no_receive else "exposure-isp-update-session/v1" if isp_update else "exposure-sweep-post-refresh-manual-session/v1" if post_refresh_manual else "exposure-sweep-awb-refresh-session/v2" if awb_refresh else "exposure-sweep-check-session/v1" if check else "exposure-sweep-session/v1"
    rule = CONTRACT_MATRIX[session_name]
    binary = PROBE / "FilmExposureSweep.3dsx"
    args = ["--auto", "--calibrate", "--offline", "--sensor=1", "--raw", "--ram",
            "--automation-fps=5", "--calibration-part=exposure-sweep"]
    if check:
        args[-1] = "--calibration-part=exposure-sweep-check"
    if awb_refresh:
        args[-1] = "--calibration-part=exposure-sweep-awb-refresh"
    if post_refresh_manual:
        args[-1] = "--calibration-part=exposure-sweep-post-refresh-manual"
    if isp_update:
        args[-1] = "--calibration-part=exposure-isp-update"
    if no_receive:
        args[-1] = "--calibration-part=no-receive-frame-update"
    if writer_window:
        args[-1] = "--calibration-part=camera-writer-window-v2"
    if hg_isolation:
        args[-1] = "--calibration-part=camera-hg-isolation"
    if ccm_normalization:
        args[-1] = "--calibration-part=camera-ccm-normalization-v3"
    if gain_matrix:
        args[-1] = "--calibration-part=camera-gain-matrix-v4"
    if exposure_rebuild:
        args[-1] = "--calibration-part=camera-exposure-rebuild-v2"
    if exposure_dense:
        args[-1]="--calibration-part=camera-exposure-dense-v1"
    source = output / "source"
    source.mkdir()
    hashes = {}
    for path in sorted((PROBE / "probes/exposure").glob("*")):
        if path.is_file():
            data = path.read_bytes()
            (source / path.name).write_bytes(data)
            hashes[path.name] = hashlib.sha256(data).hexdigest()
    record = dict(source=source_name, device=ip, arguments=args, started_unix=time.time(),
                  before=before, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  source_sha256=hashes, session_contract=rule)
    (output / "deployment.json").write_text(json.dumps(record, indent=2)+"\n")
    deploy(ip, binary, tuple(args))
    status("capturing", diagnostic=check or awb_refresh or post_refresh_manual or isp_update or no_receive or writer_window or hg_isolation or ccm_normalization or gain_matrix or exposure_rebuild or exposure_dense)
    deadline = time.monotonic() + rule["readiness_deadline_seconds"]
    while time.monotonic() < deadline:
        conn = http.client.HTTPConnection(ip, 5000, timeout=3)
        try:
            conn.request("GET", "/info")
            response = conn.getresponse()
            body = response.read(32768)
            if response.status == 200:
                if not body.startswith(b"FILMRAM 3\n"):
                    raise ValueError("unexpected recovery identity")
                prefix, entries = parse_index(body)
                (output / "ready.json").write_text(json.dumps(dict(ready_unix=time.time(), body=body.decode()))+"\n")
                status("recovering", files=len(entries), capture_complete=prefix["complete"] == "1")
                break
        except (ConnectionError, TimeoutError, OSError):
            pass  # Registered readiness check only; never re-deploy/re-measure.
        finally:
            conn.close()
        time.sleep(rule["readiness_poll_seconds"])
    else:
        raise TimeoutError("readiness deadline; device evidence retained; no measurement retry")
    retrieve(ip, output / "recovered", chunk=16384, recovery_policy="frozen-resume-v2")
    receipts = json.loads((output / "recovered/received-ram-sha256.json").read_text())
    status("recovered", files=len(receipts), bytes=sum(e["bytes"] for e in receipts),
           capture_complete=prefix["complete"] == "1", ram_failed=prefix["failed"] == "1",
           calibration_validated=False, detail_log="session.log")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("ip")
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--before", type=Path)
    diagnostic=p.add_mutually_exclusive_group()
    diagnostic.add_argument("--check", action="store_true", help="five-stage ISP diagnostic, never calibration")
    diagnostic.add_argument("--awb-refresh", action="store_true", help="independent AWB-on Force Unity Refresh experiment")
    diagnostic.add_argument("--post-refresh-manual", action="store_true", help="normalize then disable AWB before final physical writes")
    diagnostic.add_argument("--isp-update", action="store_true", help="bounded initial-value and rewrite update experiment")
    diagnostic.add_argument("--no-receive", action="store_true", help="one DMA then verified VSYNC only, with continuous DMA control")
    diagnostic.add_argument("--writer-window", action="store_true", help="one externally synchronized no-receive trace window")
    diagnostic.add_argument("--hg-isolation", action="store_true", help="HG on/off/on with bounded VSYNC phase observations")
    diagnostic.add_argument("--ccm-normalization", action="store_true", help="A355 bit6 control with HG off and bounded phase observations")
    diagnostic.add_argument("--gain-matrix", action="store_true", help="bounded fps/driver/exposure/gain matrix with adaptive observation")
    diagnostic.add_argument("--exposure-rebuild", action="store_true", help="target-specific images, adaptive gain matrix, FD ABA and continuous hold")
    diagnostic.add_argument("--exposure-dense",action="store_true",help="dense response and safe two-anchor gain images")
    a = p.parse_args()
    if not (a.check or a.awb_refresh or a.post_refresh_manual or a.isp_update or a.no_receive or a.writer_window or a.hg_isolation or a.ccm_normalization or a.gain_matrix or a.exposure_rebuild or a.exposure_dense) and a.before is None:
        p.error("full acquisition requires --before")
    run(a.ip, a.out, json.loads(a.before.read_text()) if a.before else None, check=a.check, awb_refresh=a.awb_refresh,post_refresh_manual=a.post_refresh_manual,isp_update=a.isp_update,no_receive=a.no_receive,writer_window=a.writer_window,hg_isolation=a.hg_isolation,ccm_normalization=a.ccm_normalization,gain_matrix=a.gain_matrix,exposure_rebuild=a.exposure_rebuild,exposure_dense=a.exposure_dense)
