#!/usr/bin/env python3
"""Offline evidence verification, response calibration and blind batch validation.

No camera I/O. Physical seconds use the declared cadence/fine timing estimate;
the output is reference-A EV100, not ISO certification or optical timing proof.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
import zlib

import numpy as np
from decode_exposure_state import decode, read_csv
import exposure_register_contract as reg
import exposure_sweep_contract as contract
from exposure_session import manifest_records, yuyv_rgb

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / ".tools/exposure-calibration"))


class EvidenceError(ValueError):
    pass


def require(ok, reason):
    if not ok:
        raise EvidenceError(reason)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def signature(stage, matrix):
    return {"sensor": stage["sensor"], "matrix": matrix,
            "timing": {a: v for a, v in stage["timing_raw"].items() if a != "3012"},
            **{k: stage[k] for k in ("composite_gain_raw", "analog_global_code",
                                     "analog_channel_codes", "core_digital", "isp_digital")}}


def verify(directory):
    directory = Path(directory).resolve()
    records = manifest_records(directory, "SWEEP_CONTRACT")
    require(len(records) == 1, "missing or duplicate sweep registration")
    r = records[0]
    expected = dict(source=contract.SOURCE, states=str(len(contract.STATES)),
                    points=str(len(contract.COARSE)), stages=str(contract.MAX_STAGES),
                    sensor="1", fps="5", settle="8", measured="4", retry="none", ipc_dump="0")
    expected.update(raw_x="256",raw_y="192",raw_w="128",raw_h="96")
    require(r == expected, "unknown or changed sweep contract")
    end = manifest_records(directory, "SWEEP_END")
    require(len(end) == 1 and end[0] == dict(complete="1", stages=str(contract.MAX_STAGES), restored="1"),
            "incomplete sweep or failed restoration")
    matrix = manifest_records(directory, "SWEEP_MATRIX")
    require(len(matrix) == 1 and matrix[0].get("value") in ("0", "1", "2", "3"), "invalid color matrix")
    matrix = int(matrix[0]["value"])
    report = decode(directory, frame_scope=contract.SOURCE)
    stages = report["stages"]
    planned = list(contract.stages())
    require(len(stages) == len(planned), "missing or extra stages")
    require([s["phase"] for s in stages] == list(range(3, 3 + contract.MAX_STAGES)), "nonconsecutive stages")
    native = stages[1]
    native_codes = [v["raw"] for v in native["composite_gain_raw"].values()]
    native_isp = [v["raw"] for v in native["isp_digital"].values()]
    require(all(1 <= v <= 512 for v in native_isp), "unregistered native ISP range")
    seed_plan = read_csv(directory / "calibration_plan.csv")
    frames = read_csv(directory / "frames.csv")
    by_phase = {}
    for f in frames:
        if f["measured"] == "1":
            by_phase.setdefault(int(f["phase"]), []).append(f)
    files = {n: digest(directory / n) for n in ("manifest.txt", "reads.csv", "frames.csv", "calibration_plan.csv", "frame_burst.csv", "roi.csv")}
    roi_rows = read_csv(directory / "roi.csv")
    roi_by_phase = {}
    for r in roi_rows:
        if r["tile"] == "0":
            roi_by_phase.setdefault(int(r["phase"]), []).append(r)
    reference_sig = signature(native, matrix)
    state_sigs = {}
    output = []
    raw_records = manifest_records(directory,"SWEEP_RAW")
    raw_by_phase = {}
    for raw in raw_records:
        require(int(raw["phase"]) not in raw_by_phase,"duplicate ROI frame record")
        raw_by_phase[int(raw["phase"])]=raw
    for i, (s, (label, kind, state, coarse)) in enumerate(zip(stages, planned)):
        require(s["label"] == label and s["kind"] == kind and s["sensor"] == 1, "unregistered stage order/source")
        if i == 0:
            continue  # Seed is explicitly not admitted as a locked measurement.
        require(s["proof_status"] == "verified_locked_sample", f"rejected hardware proof at {label}: {s['errors']}")
        require(s["timing_raw"]["300a"] == 2114, "wrong frame profile")
        sig = signature(s, matrix)
        if state is not None:
            require(s["timing_raw"]["3012"] == coarse, "coarse differs from registered plan")
            _, code, isp = contract.STATES[state]
            require([v["raw"] for v in s["composite_gain_raw"].values()] == ([code] * 4 if state else native_codes), "gain differs from matrix")
            require([v["raw"] for v in s["isp_digital"].values()] == ([128] * 4 if isp == "unity" else native_isp), "ISP differs from matrix")
            require(sig == state_sigs.setdefault(state, sig), "processing state changed within response bracket")
            require(sig["timing"] == reference_sig["timing"], "timing changed during sweep")
        elif label.startswith("sweep_ref_"):
            require(sig == reference_sig and s["timing_raw"]["3012"] == native["timing_raw"]["3012"], "drift reference state changed")
        # Core readback must match requested composite digital bits, not just be constant.
        for channel in reg.CHANNELS:
            require(s["core_digital"][channel]["raw"] == ((s["composite_gain_raw"][channel]["raw"] >> 9) & 7) * 256,
                    "core digital readback does not match request")
        image = directory / f"s1-p{s['phase']:03d}-{label}.yuyv"
        require(image.is_file() and image.stat().st_size == 128 * 96 * 2, f"missing raw ROI frame {label}")
        measured = sorted(by_phase[s["phase"]], key=lambda f: int(f["frame"]))
        raw_record=raw_by_phase.get(s["phase"],{})
        require(raw_record.get("bytes")==str(128*96*2), "missing native ROI length proof")
        require(zlib.crc32(image.read_bytes()) == int(raw_record.get("crc32","-1"),16), "raw ROI CRC does not match native record")
        files[image.name] = digest(image)
        center = roi_by_phase.get(s["phase"], [])
        require(len(center) == 4 and {int(r["frame"]) for r in center} == {8,9,10,11}, "missing or duplicate center frame statistics")
        means = [float(r["mean"]) for r in center]
        require(all(math.isfinite(v) for v in means), "invalid frame statistics")
        require(max(means)-min(means)<=contract.MAX_STAGE_Y_SPAN, "within-stage brightness instability (>2 Y codes)")
        output.append(dict(label=label, state=state, coarse=s["timing_raw"]["3012"],
                           image=str(image), raw_roi=[256,192,384,288], signature=sig, phase=s["phase"],
                           start_ms=min(int(f["start_ms"]) for f in measured),
                           end_ms=max(int(f["end_ms"]) for f in measured), center_y_span=max(means)-min(means)))
    # Restore targets are saved original controls, independently recovered from reads.
    reads = read_csv(directory / "reads.csv")
    saved = {r["addr"]: int(r["value"]) for r in reads if r["label"] == "sweep_original" and r["when"] == "saved" and int(r["result"], 16) < 0x80000000}
    require(set(saved) == set(reg.TARGETS), "missing original controls")
    last = seed_plan[-1]
    require(all(int(last[name]) == saved[a] for a, name in reg.TARGETS.items()), "original controls not restored")
    require([v["raw"] for v in stages[-1]["isp_digital"].values()] == native_isp, "original ISP not restored")
    burst = read_csv(directory / "frame_burst.csv")
    require(len(burst) == 24 and [int(b["frame"]) for b in burst] == list(range(24)), "invalid cadence evidence")
    require(all(b["rate"] == "5" and b["wait_result"] == b["receive_result"] == "00000000" and b["received"] == "1" and b["buffer_error"] == "0" for b in burst), "cadence capture failure")
    times = np.array([int(b["time_ms"]) for b in burst])
    require(np.all(np.diff(times) > 0), "nonmonotone cadence")
    period = float(np.mean(np.diff(times[2:]))) / 1000
    require(.15 < period < .25, "cadence does not match 5fps")
    for row in output:
        timing = row["signature"]["timing"]
        require(timing["300c"] > 0, "invalid line clock")
        row["estimated_seconds"] = (row["coarse"] + timing["3014"] / timing["300c"]) * period / 2114
    return dict(source=contract.SOURCE, capture_id=digest(directory / "manifest.txt"), directory=str(directory),
                matrix=matrix, files=files, stages=output, period_seconds=period,
                optical_time_verified=False)


def ev_reading(reading):
    require(isinstance(reading, dict), "meter reading must be an object")
    if "ev100" in reading:
        value = float(reading["ev100"])
    else:
        iso, aperture, shutter = [float(reading[k]) for k in ("iso", "f_number", "shutter_seconds")]
        require(all(math.isfinite(v) and v > 0 for v in (iso, aperture, shutter)), "invalid meter reading")
        value = math.log2(aperture**2 / shutter * 100 / iso)
    uncertainty = float(reading.get("uncertainty_ev", 0))
    require(math.isfinite(value) and math.isfinite(uncertainty) and uncertainty >= 0, "invalid EV or uncertainty")
    return value, uncertainty


def ev_bounds(reading):
    if "ev100_interval" in reading:
        bounds=reading["ev100_interval"]
        require(isinstance(bounds,list) and len(bounds)==2,"invalid EV interval")
        lo,hi=map(float,bounds)
        require(math.isfinite(lo) and math.isfinite(hi) and lo<=hi,"invalid EV interval")
        uncertainty=float(reading.get("uncertainty_ev",0))
        require(math.isfinite(uncertainty) and uncertainty>=0,"invalid interval uncertainty")
        return lo-uncertainty,hi+uncertainty
    value,uncertainty=ev_reading(reading)
    return value-uncertainty,value+uncertainty


def bind_reference(evidence, reference):
    require(reference["capture_id"] == evidence["capture_id"], "reference belongs to another capture")
    require(reference["meter"] == "A" and reference["batch_id"] and reference["target_id"], "reference identity missing")
    bl,bh = ev_bounds(reference["before"])
    al,ah = ev_bounds(reference["after"])
    gap=max(0,al-bh,bl-ah)
    require(gap <= contract.MAX_DRIFT_EV, "meter drift exceeds budget")
    # Interval overlap is NOT a proof of stable light. Image reference brackets
    # remain mandatory. Keep the before interval intact, never its midpoint.
    return bl, bh-bl


def roi_image(row, matrix, roi):
    x0, y0, x1, y1 = roi
    if "raw_roi" in row:
        rx0,ry0,rx1,ry1=row["raw_roi"]
        require(rx0<=x0<x1<=rx1 and ry0<=y0<y1<=ry1,"requested ROI outside recorded raw region")
        data=Path(row["image"]).read_bytes()
        require(len(data)==(rx1-rx0)*(ry1-ry0)*2,"truncated raw ROI")
        q=np.frombuffer(data,np.uint8).reshape(ry1-ry0,(rx1-rx0)//2,4).astype(float)
        yy=np.empty((ry1-ry0,rx1-rx0));yy[:,::2]=q[:,:,0];yy[:,1::2]=q[:,:,2]
        limited,hd=matrix>=2,matrix%2==1
        yy=(yy-(16 if limited else 0))/(219 if limited else 255)
        u=np.repeat((q[:,:,1]-128)/(224 if limited else 255),2,axis=1)
        v=np.repeat((q[:,:,3]-128)/(224 if limited else 255),2,axis=1)
        rgb=np.stack([yy+(1.5748 if hd else 1.402)*v,
                      yy-(.187324 if hd else .344136)*u-(.468124 if hd else .714136)*v,
                      yy+(1.8556 if hd else 1.772)*u],axis=-1)
        return np.rint(np.clip(rgb[y0-ry0:y1-ry0,x0-rx0:x1-rx0],0,1)*255).astype(np.uint8)
    return np.rint(yuyv_rgb(Path(row["image"]), matrix)[y0:y1, x0:x1] * 255).astype(np.uint8)


def log_radiance(image, seconds, response, domain):
    valid = np.ones(image.shape[:2], dtype=bool)
    result = np.empty(image.shape, dtype=float)
    for c in range(3):
        lo, hi = domain[c]
        valid &= (image[:, :, c] >= lo) & (image[:, :, c] <= hi)
        result[:, :, c] = np.log2(response[image[:, :, c], c]) - math.log2(seconds)
    return result, valid


def monotonic_response(images, times):
    """Fit log response increments >=epsilon; eliminate per-pixel irradiance.

    g(z_ij) = log(t_j) + log(E_i). Weighted centering over exposures
    eliminates E_i, avoiding any constant/uniform-target assumption. Positive
    increments constrain the fit itself; no post-fit editing or unit slope.
    """
    from scipy.optimize import lsq_linear
    rule=contract.CONTRACT_MATRIX["exposure-sweep-analysis/v1"]
    h,w,_=images[0].shape
    ys=np.linspace(0,h-1,10,dtype=int);xs=np.linspace(0,w-1,10,dtype=int)
    samples=np.stack([im[np.ix_(ys,xs)].reshape(-1,3) for im in images])
    logt=np.log(np.asarray(times))
    # g(0)=0 is arbitrary: the irradiance terms absorb any common log offset.
    cumulative=(np.arange(255)[None,:]<np.arange(256)[:,None]).astype(float)
    smooth=np.diff(np.eye(255),axis=0)
    weight=np.minimum(np.arange(1,255),255-np.arange(1,255))
    regularizer=smooth*(rule["smoothing"]*weight[:,None])
    responses=[]
    for channel in range(3):
        matrices=[];targets=[]
        for i in range(samples.shape[1]):
            z=samples[:,i,channel].astype(int)
            valid=(z>=4)&(z<=250)
            if np.count_nonzero(valid)<2:continue
            z=z[valid];t=logt[valid];weights=np.minimum(z,255-z).astype(float)
            design=cumulative[z]
            a=(design-np.average(design,axis=0,weights=weights**2))*weights[:,None]
            b=(t-np.average(t,weights=weights**2))*weights
            matrices.append(a);targets.append(b)
        require(len(matrices)>=20,"insufficient paired training pixels")
        design=np.vstack(matrices+[regularizer]);target=np.concatenate(targets+[np.zeros(254)])
        result=lsq_linear(design,target,bounds=(rule["min_log_increment"],np.inf),
                          method="bvls",tol=1e-8,max_iter=rule["solver_iterations"])
        require(result.success and np.all(np.isfinite(result.x)),"monotonic response solver did not converge")
        log_response=np.r_[0,np.cumsum(result.x)]
        log_response-=log_response[128]
        responses.append(np.exp(log_response))
    return np.stack(responses,axis=1)


def fit_response(images, times, holdout_indices=contract.HOLDOUT_INDICES):
    require(len(images) == len(times) == len(contract.COARSE), "incomplete response sweep")
    require(all(math.isfinite(float(t)) and t>0 for t in times) and all(b>a for a,b in zip(times,times[1:])), "invalid or nonmonotone exposure times")
    require(all(isinstance(im,np.ndarray) and im.dtype==np.uint8 and im.ndim==3 and im.shape[-1]==3 and im.shape==images[0].shape for im in images), "inconsistent RGB training images")
    require(images[0].shape[0]>=10 and images[0].shape[1]>=10, "insufficient spatial samples")
    require(tuple(holdout_indices)==contract.HOLDOUT_INDICES,"unregistered holdout partition")
    training = [i for i in range(len(images)) if i not in holdout_indices]
    require(len(training) >= 4, "insufficient independent training exposures")
    response = monotonic_response([images[i] for i in training],[times[i] for i in training])
    require(np.all(np.isfinite(response)) and np.all(response > 0), "nonfinite inverse response")
    # Declare domains from measured training codes; do not extrapolate dark/clipped ends.
    stack = np.stack([images[i] for i in training])
    domain = []
    for c in range(3):
        values = stack[..., c]
        values = values[(values >= 4) & (values <= 250)]
        require(values.size > 0, "no unclipped training codes")
        lo, hi = int(values.min()), int(values.max())
        require(hi - lo >= 32, "insufficient measured code coverage")
        require(np.all(np.diff(response[lo:hi+1, c]) > 0), "nonmonotone inverse response")
        domain.append([lo, hi])
    logs, masks = zip(*(log_radiance(im, t, response, domain) for im, t in zip(images, times)))
    # Per-pixel training radiance. Inverting first avoids inverse(mean(code)).
    training_stack = np.stack([np.where(masks[i][..., None], logs[i], np.nan) for i in training])
    count = np.sum(np.isfinite(training_stack[..., 0]), axis=0)
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)  # uncovered pixels stay NaN and fail coverage below
        baseline = np.nanmedian(training_stack, axis=0)
    checks = []
    for i in holdout_indices:
        mask = masks[i] & (count >= 2)
        fraction = float(mask.mean())
        error = np.median(logs[i][mask] - baseline[mask], axis=0) if fraction >= .8 else None
        checks.append(dict(index=i, common_pixel_fraction=fraction,
                           channel_error_ev=error.tolist() if error is not None else None,
                           passed=error is not None and bool(np.max(np.abs(error)) <= contract.MAX_ERROR_EV)))
    usable = [c for c in checks if c["channel_error_ev"] is not None]
    passed = len(usable) >= 2 and all(c["passed"] for c in usable)
    return dict(response=response.tolist(), domain=domain, training=training,
                holdout=checks, passed=passed, baseline=baseline, count=count)


def fit_batch(evidence, roi):
    states = []
    for state, (name, _, _) in enumerate(contract.STATES):
        rows = [r for r in evidence["stages"] if r["state"] == state]
        try:
            images = [roi_image(r, evidence["matrix"], roi) for r in rows]
            fit = fit_response(images, [r["estimated_seconds"] for r in rows])
            baseline, count = fit.pop("baseline"), fit.pop("count")
            valid = count >= 2
            require(bool(np.all(valid)), "whole metering ROI lacks two unclipped training samples per pixel")
            # This scale is a relative RGB radiance statistic, not an ISO number.
            anchor = np.mean(np.exp2(baseline[valid]), axis=0)
            states.append(dict(state=state, name=name, signature=rows[0]["signature"],
                               anchor_radiance=anchor.tolist(), **fit))
        except EvidenceError as error:
            states.append(dict(state=state, name=name, passed=False, reason=str(error)))
    # Fixed-setting interleaved references check the whole run, using the measured response.
    references = [r for r in evidence["stages"] if r["label"].startswith("sweep_ref_")]
    drift = []
    native = states[0]
    if "response" in native:
        response, domain = np.array(native["response"]), native["domain"]
        baseline, baseline_mask = log_radiance(roi_image(references[0], evidence["matrix"], roi), references[0]["estimated_seconds"], response, domain)
        for row in references:
            log, mask = log_radiance(roi_image(row, evidence["matrix"], roi), row["estimated_seconds"], response, domain)
            mask &= baseline_mask
            error = np.median(log[mask] - baseline[mask], axis=0) if mask.mean() >= .8 else None
            drift.append(dict(label=row["label"], channel_ev=error.tolist() if error is not None else None,
                              passed=error is not None and bool(np.max(np.abs(error)) <= contract.MAX_DRIFT_EV)))
    speed = []
    # Effective pipeline response at explicit output levels, not a scalar gain-code formula.
    for state in states:
        if not state.get("passed"):
            continue
        for code in (64,128,192):
            if all(lo<=code<=hi for lo,hi in state["domain"]):
                t=np.array(state["response"])[code] / np.array(state["anchor_radiance"])
                speed.append(dict(state=state["state"],output_code=code,
                                  RGB_target_seconds=t.tolist(),kind="model_prediction_not_optical_time"))
    return dict(schema="exposure-response/v2", capture_id=evidence["capture_id"], roi=roi,
                states=states, drift=drift, drift_passed=len(drift)==26 and all(d["passed"] for d in drift),
                target_response=speed,
                evidence_files=evidence["files"], optical_time_verified=False,
                estimator="per-pixel inverse response, RGB radiance mean; fixed target/ROI",
                method="monotonic log-response bounded least squares; 100 deterministic pixels; lambda10; held-out indices 1,3,5,7",
                analysis_source_sha256=digest(__file__), analysis_contract=contract.CONTRACT_MATRIX["exposure-sweep-analysis/v1"])


def validation_drift(evidence, roi):
    """Fit test-native response ONLY to check stability, never to predict its EV."""
    rows = [r for r in evidence["stages"] if r["state"] == 0]
    fit = fit_response([roi_image(r, evidence["matrix"], roi) for r in rows],
                       [r["estimated_seconds"] for r in rows])
    require(fit["passed"], "test batch native response cannot verify drift")
    references = [r for r in evidence["stages"] if r["label"].startswith("sweep_ref_")]
    first, first_mask = log_radiance(roi_image(references[0], evidence["matrix"], roi), references[0]["estimated_seconds"],
                                    np.array(fit["response"]), fit["domain"])
    results = []
    for row in references:
        log, mask = log_radiance(roi_image(row, evidence["matrix"], roi), row["estimated_seconds"],
                                 np.array(fit["response"]), fit["domain"])
        mask &= first_mask
        require(mask.mean() >= .8, "test drift reference outside response domain")
        error = float(np.max(np.abs(np.median(log[mask]-first[mask],axis=0))))
        results.append(error)
    require(len(results)==26 and max(results)<=contract.MAX_DRIFT_EV, "test illumination drift")
    return results


def predict_stage(model_state, row, matrix, roi, anchor_ev):
    require(model_state.get("passed"), "state failed response validation")
    require(model_state["signature"] == row["signature"], "unknown gain/ISP/timing state")
    image = roi_image(row, matrix, roi)
    log, mask = log_radiance(image, row["estimated_seconds"], np.array(model_state["response"]), model_state["domain"])
    require(float(mask.mean()) >= .8, "prediction outside calibrated pixel domain")
    # Fixed whole-ROI mean requires all pixels, otherwise selection changes the meter.
    require(bool(np.all(mask)), "full requested ROI not covered; no selective metering")
    radiance = np.mean(np.exp2(log), axis=(0, 1))
    channel_ev = anchor_ev + np.log2(radiance / np.array(model_state["anchor_radiance"]))
    ev = math.log2(float(np.dot(np.exp2(channel_ev), [.2126, .7152, .0722])))
    return dict(ev100=ev, channel_ev100=channel_ev.tolist(),
                equivalent_A_seconds_iso100_f28=7.84 / 2**ev)


def validate_model(model):
    require(model.get("schema")=="exposure-response/v2","unknown exposure model schema")
    require(len(model.get("states",[]))==len(contract.STATES),"incomplete model state matrix")
    for i,state in enumerate(model["states"]):
        require(state.get("state")==i,"model state order mismatch")
        if not state.get("passed"):continue
        r=np.array(state["response"]);anchor=np.array(state["anchor_radiance"])
        require(r.shape==(256,3) and np.all(np.isfinite(r)) and np.all(r>0),"invalid response artifact")
        require(anchor.shape==(3,) and np.all(np.isfinite(anchor)) and np.all(anchor>0),"invalid anchor artifact")
        require(len(state["domain"])==3,"invalid response domain")
        for ch,(lo,hi) in enumerate(state["domain"]):
            require(4<=lo<hi<=250 and np.all(np.diff(r[lo:hi+1,ch])>0),"invalid monotonic domain")


def compare(model, train_ref, evidence, test_ref):
    require(model["capture_id"] != evidence["capture_id"], "validation batch is training batch")
    validate_model(model)
    require(model["capture_id"] == train_ref["capture_id"], "training reference mismatch")
    require(train_ref["batch_id"] != test_ref["batch_id"], "validation needs an independent batch")
    require(train_ref["target_id"] == test_ref["target_id"], "target identity changed")
    anchor_ev, anchor_width = bind_reference({"capture_id": model["capture_id"]}, train_ref)
    actual_ev, actual_width = bind_reference(evidence, test_ref)
    separation=max(actual_ev-(anchor_ev+anchor_width),anchor_ev-(actual_ev+actual_width))
    require(separation >= 1, "cross-illumination test requires at least 1EV guaranteed separation")
    require(model["drift_passed"], "training illumination drift not verified")
    drift = validation_drift(evidence, model["roi"])
    rows = []
    for row in evidence["stages"]:
        if row["state"] is None:
            continue
        state = model["states"][row["state"]]
        try:
            prediction = predict_stage(state, row, evidence["matrix"], model["roi"], anchor_ev)
            error = prediction["ev100"] - actual_ev
            interval=[error-actual_width,error+anchor_width]
            predicted_interval=[prediction["ev100"],prediction["ev100"]+anchor_width]
            if anchor_width:
                prediction["channel_ev100_intervals"]=[[v,v+anchor_width] for v in prediction.pop("channel_ev100")]
                prediction.pop("ev100");prediction.pop("equivalent_A_seconds_iso100_f28")
            rows.append(dict(label=row["label"], state=row["state"], **prediction,
                             error_ev=error if not (actual_width or anchor_width) else None,
                             error_ev_interval=interval,
                             predicted_ev100_interval=predicted_interval,
                             passed=max(map(abs,interval)) <= contract.MAX_ERROR_EV))
        except EvidenceError as error:
            rows.append(dict(label=row["label"], state=row["state"], passed=False, reason=str(error)))
    summary = []
    for state in range(len(contract.STATES)):
        applicable = [r for r in rows if r["state"] == state and "error_ev_interval" in r]
        summary.append(dict(state=state, validated_points=len(applicable),
                            passed=len(applicable)>=2 and all(r["passed"] for r in applicable)))
    return dict(schema="exposure-transfer/v1", training_capture=model["capture_id"],
                test_capture=evidence["capture_id"], test_reference=test_ref, results=rows,
                states=summary, all_states_passed=all(s["passed"] for s in summary),
                test_reference_used_for_fit=False, test_drift_ev=drift, absolute_iso_verified=False)


def predict_batch(model, train_ref, evidence):
    validate_model(model)
    require(model["capture_id"]==train_ref["capture_id"], "training reference mismatch")
    require(model["capture_id"]!=evidence["capture_id"], "prediction must use an independent capture")
    require(model["drift_passed"], "training light drift failed")
    anchor,width=bind_reference({"capture_id":model["capture_id"]},train_ref)
    rows=[]
    for row in evidence["stages"]:
        if row["state"] is None:continue
        try:
            prediction=predict_stage(model["states"][row["state"]],row,evidence["matrix"],model["roi"],anchor)
            prediction["ev100_interval"]=[prediction["ev100"],prediction["ev100"]+width]
            prediction["equivalent_A_seconds_interval"]=[7.84/2**(prediction["ev100"]+width),7.84/2**prediction["ev100"]]
            if width:
                prediction.pop("ev100");prediction.pop("equivalent_A_seconds_iso100_f28")
                prediction["channel_ev100_intervals"]=[[v,v+width] for v in prediction.pop("channel_ev100")]
            rows.append(dict(label=row["label"],state=row["state"],status="prediction_awaiting_independent_reference",**prediction))
        except EvidenceError as error:
            rows.append(dict(label=row["label"],state=row["state"],status="rejected",reason=str(error)))
    return dict(schema="exposure-blind-predictions/v1",training_capture=model["capture_id"],
                capture_id=evidence["capture_id"],results=rows,test_reference_used=False,
                transfer_validated=False)


def write_new(path, result):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x") as f:
        json.dump(result, f, indent=2, ensure_ascii=False, allow_nan=False)
        f.write("\n")


def parse_meter(text):
    """ISO,f-number,seconds (fractions accepted), or EV100=number."""
    from fractions import Fraction
    if text.startswith("EV100="):
        if ":" in text[6:]:
            result=dict(ev100_interval=[float(v) for v in text[6:].split(":")],entered=text)
        else:
            result = dict(ev100=float(text[6:]), entered=text)
    else:
        parts = text.split(",")
        require(len(parts)==3, "meter format is ISO,f-number,shutter; e.g. 100,2.8,1/4")
        result = dict(iso=float(parts[0]),f_number=float(parts[1]),
                      shutter_seconds=float(Fraction(parts[2])),entered=text)
    ev_bounds(result)
    return result


def summary(result):
    lines = ["# Exposure verification", "", "All results are local diagnostic evidence. No absolute ISO or optical-time claim.", ""]
    if result.get("schema")=="exposure-response/v2":
        lines += [f"Capture: `{result['capture_id']}`", f"Light drift passed: {result['drift_passed']}", "",
                  "|State|Response passed|Held-out maximum EV error|Reason|", "|---|---|---|---|"]
        for state in result["states"]:
            errors=[abs(e) for p in state.get("holdout",[]) for e in (p["channel_error_ev"] or [])]
            worst=f"{max(errors):.5f}" if errors else "unavailable"
            lines.append(f"|{state['name']}|{state['passed']}|{worst}|{state.get('reason','')}|")
    elif result.get("schema")=="exposure-transfer/v1":
        lines += [f"All registered states passed: {result['all_states_passed']}", "Test references were not used for fitting.", "",
                  "|State|Validated points|Passed|", "|---|---|---|"]
        for state in result["states"]:
            lines.append(f"|{contract.STATES[state['state']][0]}|{state['validated_points']}|{state['passed']}|")
    else:
        lines += [f"Verified capture: `{result.get('capture_id','')}`"]
    return "\n".join(lines)+"\n"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)
    for cmd in ("verify", "fit", "compare", "reference", "predict"):
        q = sub.add_parser(cmd)
        q.add_argument("directory", type=Path)
        q.add_argument("--out", required=True, type=Path)
        if cmd == "fit":
            q.add_argument("--roi", nargs=4, type=int, default=[256,192,384,288])
        if cmd in ("compare","predict"):
            q.add_argument("--model", required=True, type=Path)
            q.add_argument("--training-reference", required=True, type=Path)
        if cmd == "compare":
            q.add_argument("--reference", required=True, type=Path)
        if cmd == "reference":
            q.add_argument("--batch", required=True)
            q.add_argument("--target", required=True)
            q.add_argument("--before", required=True)
            q.add_argument("--after", required=True)
            q.add_argument("--uncertainty-ev",type=float,default=0)
    a = p.parse_args()
    evidence = verify(a.directory)
    if a.command == "verify":
        result = evidence
    elif a.command == "fit":
        x0,y0,x1,y1=a.roi
        require(0<=x0<x1<=640 and 0<=y0<y1<=480, "invalid ROI")
        result = fit_batch(evidence, a.roi)
    elif a.command == "reference":
        result = dict(capture_id=evidence["capture_id"],batch_id=a.batch,target_id=a.target,meter="A",
                      before={**parse_meter(a.before),"uncertainty_ev":a.uncertainty_ev},
                      after={**parse_meter(a.after),"uncertainty_ev":a.uncertainty_ev})
        # Preserve a failed drift pair for inspection instead of rewriting its values.
        try:
            ev, uncertainty=bind_reference(evidence,result)
            result.update(binding_passed=True,anchor_ev100_interval=[ev,ev+uncertainty],
                          external_drift_proven=False,
                          note="intervals compatible; image reference bracket proof still required")
        except EvidenceError as error:
            result.update(binding_passed=False,reason=str(error))
    elif a.command=="predict":
        result=predict_batch(json.loads(a.model.read_text()),json.loads(a.training_reference.read_text()),evidence)
    else:
        result = compare(json.loads(a.model.read_text()), json.loads(a.training_reference.read_text()),
                         evidence, json.loads(a.reference.read_text()))
    write_new(a.out, result)
    if a.command in ("fit","compare"):
        with Path(str(a.out)+".md").open("x") as f:f.write(summary(result))
    print(json.dumps({k:v for k,v in result.items() if k in ("capture_id","drift_passed","all_states_passed")},ensure_ascii=False))


if __name__ == "__main__":
    main()
