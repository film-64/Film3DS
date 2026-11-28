"""Freeze the UI sources with LUTs, then deploy that verified build once."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import zlib

from camera_runtime_contract import CONTRACT_MATRIX
from verify_3dsx import verify

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def stage(out, luts, grain_texture=None):
    assert CONTRACT_MATRIX["film-camera-ui-package/v1"]["compute"] is False
    cubes = sorted(p for p in luts.iterdir() if p.is_file() and p.suffix.lower()==".cube")
    if not cubes:
        raise ValueError("require standalone .cube LUT files")
    if any(Path(str(cube)+".f3a").exists() for cube in cubes):
        raise ValueError("Sidecar adapters are SD-only: copy matching .cube + .cube.f3a to SD luts; bundling would omit the adapter")
    if out.exists():
        raise FileExistsError(out)
    out.mkdir(parents=True)
    for directory in ("source", "include", "romfs"):
        shutil.copytree(ROOT / directory, out / directory, ignore=shutil.ignore_patterns(".DS_Store"))
    (out / "tools").mkdir()
    for name in ("build_arm_jpeg.sh", "verify_arm11.sh"):
        shutil.copy2(ROOT / "tools" / name, out / "tools" / name)
    for name in ("Makefile", "Makefile.3ds"):
        shutil.copy2(ROOT / name, out / name)
    if grain_texture is not None:
        shutil.copy2(grain_texture,out/"romfs/grain/pan400.f3gt")
    packed = out / "romfs/luts"
    packed.mkdir(exist_ok=True)
    for cube in cubes:
        shutil.copy2(cube, packed / cube.name)
    # Reuse the already-pinned JPEG source, never old application objects.
    jpeg = ROOT / "build-3ds/jpeg-arm11/source"
    if jpeg.is_dir():
        shutil.copytree(jpeg, out / "build-3ds/jpeg-arm11/source")
    hashes = {str(p.relative_to(out)): sha(p) for directory in ("source", "include", "romfs", "tools")
              for p in sorted((out / directory).rglob("*")) if p.is_file()}
    hashes.update({name: sha(out / name) for name in ("Makefile", "Makefile.3ds")})
    manifest = dict(source="film-camera-ui-package/v1", ui=True, lut_count=len(cubes),
                    files=hashes, runtime_contract=CONTRACT_MATRIX)
    (out / "package.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Staged UI + {len(cubes)} LUTs: {out}")

def checked_manifest(out):
    manifest = json.loads((out / "package.json").read_text())
    if manifest["source"] != "film-camera-ui-package/v1" or not manifest["ui"] or not manifest["lut_count"]:
        raise ValueError("not a UI/LUT package")
    for name, expected in manifest["files"].items():
        if sha(out / name) != expected:
            raise ValueError(f"frozen package changed: {name}")
    return manifest

def build(out):
    checked_manifest(out)
    if not os.environ.get("DEVKITPRO"):
        raise ValueError("DEVKITPRO must point to a usable 3DS SDK")
    subprocess.run(["make", "-j2", "3ds"], cwd=out, check=True)
    checked_manifest(out)
    app = out / "Film3DS.3dsx"
    verify(app)
    arm = Path(os.environ.get("DEVKITARM", str(Path(os.environ["DEVKITPRO"]) / "devkitARM")))
    subprocess.run(["sh", "tools/verify_arm11.sh", "Film3DS.elf", str(arm / "bin/arm-none-eabi-")],
                   cwd=out, check=True)
    payload = app.read_bytes()
    for resource in (out / "romfs").rglob("*"):
        if resource.is_file() and resource.read_bytes() not in payload:
            raise ValueError(f"ROMFS asset missing from app: {resource.name}")
    (out / "build.json").write_text(json.dumps(dict(
        app_sha256=sha(app), source_manifest_sha256=sha(out / "package.json"),
        arm11_verified=True, romfs_verified=True), indent=2) + "\n")
    print("Built and verified UI, ARM11 and embedded LUT/grain resources:", app)

def exact(sock, size):
    result = bytearray()
    while len(result) < size:
        part = sock.recv(size - len(result))
        if not part:
            raise EOFError("incomplete NetLoader acknowledgement")
        result.extend(part)
    return bytes(result)

def prepare_deployment(receipt, record, readiness_note, receiver_reset_note=None):
    """One new invocation after explicit readiness, never a network retry loop."""
    if not receipt.exists():
        if receiver_reset_note:
            raise FileExistsError("receiver-reset deployment requires a prior failed payload receipt")
        return
    policy = CONTRACT_MATRIX["film-camera-ui-deploy/v1"]
    assert policy["new_invocation"] == "explicit_user_readiness_after_connect_failure_without_transfer_only"
    previous = json.loads(receipt.read_text())
    same = (previous.get("sha256") == record["sha256"]
            and previous.get("source_manifest_sha256") == record["source_manifest_sha256"])
    connect_ready = (readiness_note and readiness_note.strip() and previous.get("status") == "failed"
            and previous.get("failure_phase") == "connect" and previous.get("transfer_started") is False
            and same)
    reset_policy = CONTRACT_MATRIX["film-camera-ui-receiver-reset-deploy/v1"]
    assert not reset_policy["compute"] and reset_policy["retries"] == 0
    reset_ready = (receiver_reset_note and receiver_reset_note.strip() and same
            and previous.get("status") == "failed" and previous.get("failure_phase") == "payload"
            and previous.get("transfer_started") is True)
    if not (connect_ready or reset_ready):
        raise FileExistsError("requires explicit readiness after zero-transfer connect failure, or explicit receiver restart after payload failure; same binary and manifest only")
    archive = receipt.parent / f"deployment-failed-{len(list(receipt.parent.glob('deployment-failed-*.json'))) + 1:02}.json"
    with archive.open("x") as file:
        file.write(receipt.read_text())
    if reset_ready:
        record["source"] = "film-camera-ui-receiver-reset-deploy/v1"
        record["receiver_reset_note"] = receiver_reset_note
        record["prior_failure_sha256"] = sha(archive)
    else:
        record["readiness_note"] = readiness_note

def deploy(out, ip, port, readiness_note=None, receiver_reset_note=None):
    policy = CONTRACT_MATRIX["film-camera-ui-deploy/v1"]
    assert policy["deployments"] == 1 and policy["retries"] == 0
    manifest = checked_manifest(out)
    app = out / "Film3DS.3dsx"
    built = json.loads((out / "build.json").read_text())
    if built != dict(app_sha256=sha(app), source_manifest_sha256=sha(out / "package.json"),
                     arm11_verified=True, romfs_verified=True):
        raise ValueError("build receipt does not match frozen UI/LUT package")
    verify(app)
    payload = app.read_bytes()
    name = b"Film3DS-HW15-UI.3dsx"
    record = dict(source="film-camera-ui-deploy/v1", ip=ip, port=port,
                  sha256=sha(app), bytes=len(payload), lut_count=manifest["lut_count"],
                  source_manifest_sha256=sha(out / "package.json"), status="started",
                  failure_phase="connect", transfer_started=False)
    receipt = out / "deployment.json"
    prepare_deployment(receipt, record, readiness_note, receiver_reset_note)
    receipt.write_text(json.dumps(record, indent=2) + "\n")
    try:
        with socket.create_connection((ip, port), timeout=10) as sock:
            sock.settimeout(45)
            record.update(failure_phase="header", transfer_started=True)
            sock.sendall(struct.pack("<I", len(name)) + name + struct.pack("<I", len(payload)))
            status = struct.unpack("<i", exact(sock, 4))[0]
            if status:
                raise ValueError(f"NetLoader header rejected: {status}")
            record["failure_phase"] = "payload"
            compressed = zlib.compress(payload)
            for position in range(0, len(compressed), 16384):
                chunk = compressed[position:position + 16384]
                sock.sendall(struct.pack("<I", len(chunk)) + chunk)
            status = struct.unpack("<i", exact(sock, 4))[0]
            if status:
                raise ValueError(f"NetLoader payload rejected: {status}")
            record["failure_phase"] = "launch"
            arguments = b"sdmc:/3ds/Film3DS-HW15-UI.3dsx\0"
            sock.sendall(struct.pack("<I", len(arguments)) + arguments)
    except (OSError, EOFError, ValueError) as error:
        record.update(status="failed", error_type=type(error).__name__, error=str(error))
        receipt.write_text(json.dumps(record, indent=2) + "\n")
        raise
    record["status"] = "NetLoader_payload_accepted_launch_requested"
    record["failure_phase"] = None
    receipt.write_text(json.dumps(record, indent=2) + "\n")
    print(record["status"], record["sha256"])

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("stage", "build", "deploy"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--luts", type=Path, default=ROOT / "build/hardware/film-lab/romfs/luts")
    parser.add_argument("--grain-texture",type=Path)
    parser.add_argument("--ip")
    parser.add_argument("--port", type=int, default=17491)
    parser.add_argument("--readiness-note", help="actual user readiness reply after a prior zero-transfer connection failure")
    parser.add_argument("--receiver-reset-note",help="actual user confirmation that NetLoader restarted after a failed payload; not permission to retry a live receiver")
    args = parser.parse_args()
    if args.action == "stage":
        stage(args.out.resolve(), args.luts,args.grain_texture)
    elif args.action == "build":
        build(args.out.resolve())
    else:
        if not args.ip:
            parser.error("deploy requires --ip")
        deploy(args.out.resolve(), args.ip, args.port, args.readiness_note,args.receiver_reset_note)
