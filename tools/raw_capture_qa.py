"""One explicitly requested on-device production RAW capture; collect evidence."""
import argparse
import hashlib
import ipaddress
import json
from pathlib import Path
import secrets
import socket
import subprocess

from camera_runtime_contract import CONTRACT_MATRIX
from camera_ui15_package import checked_manifest, sha
from verify_3dsx import verify

SOURCE = "film-raw-capture-automation-deploy/v1"


def collect(stream, token, out, log):
    def line():
        data = stream.readline(513)
        if not data.endswith(b"\n") or len(data) > 512:
            raise ValueError("incomplete or oversized RAW QA line")
        result = data.decode("ascii").rstrip("\n")
        log.write(result + "\n")
        log.flush()
        return result
    if line() != f"F3RAWQA1 {token}":
        raise ValueError("RAW QA token mismatch")
    result = None
    raw_sha = None
    for _ in range(4096):
        text = line()
        if text.startswith("RESULT "):
            if result is not None:
                raise ValueError("duplicate result")
            result = dict(field.split("=", 1) for field in text.split()[1:])
            if result.get("raw_bytes") != ("614400" if result.get("status") == "ok" else "0"):
                raise ValueError("inconsistent capture result")
        elif text == "RAW_BYTES 614400":
            if result != {"status": "ok", "raw_bytes": "614400"} or raw_sha:
                raise ValueError("unexpected RAW payload")
            data = bytearray()
            partial=out / "capture.bin.part"
            with partial.open("xb") as file:
                while len(data) < 614400:
                    chunk = stream.read1(min(16384,614400-len(data)))
                    if not chunk:
                        raise ValueError("truncated RAW payload")
                    data.extend(chunk);file.write(chunk);file.flush()
                    (out / "raw-progress.json").write_text(json.dumps(dict(bytes_received=len(data),expected=614400))+"\n")
            if stream.read(1) != b"\n" or any(v > 3 for v in data[1::2]):
                raise ValueError("invalid RAW high8/low2 encoding")
            if len(set(data[0::2])) < 2:
                raise ValueError("uniform RAW payload cannot prove sensor data")
            raw_sha = hashlib.sha256(data).hexdigest()
            partial.rename(out / "capture.bin")
        elif text == "DONE":
            if result is None or (result["status"] == "ok") != bool(raw_sha):
                raise ValueError("incomplete capture evidence")
            return dict(result, raw_sha256=raw_sha, hardware_observed=True)
    raise ValueError("RAW QA trace exceeds contract bound")


def prepare_receipt(receipt, record, readiness_note):
    if receipt.exists():
        previous = json.loads(receipt.read_text())
        if (not readiness_note or not readiness_note.strip() or previous.get("status") != "failed"
                or any(previous.get(key) != record[key] for key in ("app_sha256", "manifest_sha256"))):
            raise FileExistsError("prior attempt requires explicit receiver readiness and identical frozen build")
        archive = receipt.with_name(f"rawqa-failed-{len(list(receipt.parent.glob('rawqa-failed-*.json'))) + 1:02}.json")
        with archive.open("x") as file:
            file.write(receipt.read_text())
        record.update(readiness_note=readiness_note, prior_failure_sha256=sha(archive))
    else:
        with receipt.open("x") as file:
            json.dump(record, file, indent=2)
    receipt.write_text(json.dumps(record, indent=2) + "\n")


def deploy(package, target, out, link, readiness_note=None, recover=None, recover_only=False):
    if recover is not None and (len(recover)!=32 or any(c not in "0123456789abcdef" for c in recover)):
        raise ValueError("invalid recovery token")
    if recover_only and not recover:
        raise ValueError("recover-only requires explicit prior token")
    target = str(ipaddress.IPv4Address(target))
    policy = CONTRACT_MATRIX[SOURCE]
    assert policy["deployments"] == 1 and policy["retries"] == 0
    checked_manifest(package)
    app = package / "Film3DS.3dsx"
    build = json.loads((package / "build.json").read_text())
    if build != dict(app_sha256=sha(app), source_manifest_sha256=sha(package / "package.json"),
                     arm11_verified=True, romfs_verified=True):
        raise ValueError("frozen build receipt mismatch")
    verify(app)
    if recover and "film-raw-capture-trace-recover/v1" not in checked_manifest(package)["runtime_contract"]:
        raise ValueError("frozen package does not register trace recovery")
    token = secrets.token_hex(16)
    receipt = package / "rawqa-deployment.json"
    # A failed upload is not permission to launch again or change packages.
    record = dict(source=SOURCE, token=token, target=target, app_sha256=sha(app),
                  manifest_sha256=sha(package / "package.json"), status="prepared", recover=recover, recover_only=recover_only)
    if out.exists():
        raise FileExistsError(out)
    prepare_receipt(receipt, record, readiness_note)
    out.mkdir(parents=True, exist_ok=False)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
            route.connect((target, 17491))
            local = route.getsockname()[0]
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((local, 17491))
            listener.listen(1)
            listener.settimeout(30)
            record["status"] = "upload_started"
            receipt.write_text(json.dumps(record, indent=2) + "\n")
            print(f"RAW QA return channel {local}:17491; one launch to {target}", flush=True)
            with (out / "upload.log").open("wb") as upload:
                command=[str(link), "-a", target, "-r", "1", str(app), f"f3rawqa={token}"]
                if recover:command.append(f"f3rawrecover={recover}")
                if recover_only:command.append("f3rawrecover-only")
                subprocess.run(command, stdout=upload, stderr=subprocess.STDOUT, check=True, timeout=120)
            record["status"] = "uploaded_waiting_for_device_report"
            receipt.write_text(json.dumps(record, indent=2) + "\n")
            connection, peer = listener.accept()
            with connection:
                if peer[0] != target:
                    raise ValueError("unexpected report peer")
                connection.settimeout(60)
                with connection.makefile("rb") as stream, (out / "trace.txt").open("w") as log:
                    report = collect(stream, token, out, log)
                (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
                connection.sendall(b"K")
            record.update(status="device_report_received", capture_status=report["status"])
            print(json.dumps(report, indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        record.update(status="failed", error=str(error))
        raise
    finally:
        receipt.write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--ip", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--link", type=Path, default=Path("/Users/a64/Documents/Codex/.tools/film3ds-3dslink/3dslink"))
    parser.add_argument("--recover", help="prior 32 hex diagnostic token whose SD trace should be returned")
    parser.add_argument("--recover-only", action="store_true", help="recover prior trace without calling camera")
    parser.add_argument("--readiness-note", help="actual user confirmation of restarted NetLoader; identical frozen build only")
    args = parser.parse_args()
    deploy(args.package.resolve(), args.ip, args.out.resolve(), args.link, args.readiness_note, args.recover, args.recover_only)
