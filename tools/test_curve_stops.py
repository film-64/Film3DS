"""Measure all 11 curve stops in the deployed C core and produce standalone figures.

Test-only: does not alter curve constants, runtime contracts, or camera settings.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile

cache = Path(tempfile.gettempdir()) / "film3ds-curve-plot-cache"
cache.mkdir(exist_ok=True)
os.environ.setdefault("MPLCONFIGDIR", str(cache))
os.environ.setdefault("XDG_CACHE_HOME", str(cache))

import numpy as np
from PIL import Image, ImageDraw
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
WEIGHTS = np.array([.2126, .7152, .0722])


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def luma(image):
    return np.asarray(image, dtype=float) @ WEIGHTS


def plot_curves(directory, looks):
    fig, axes = plt.subplots(3, len(looks), figsize=(16, 10), sharex=True, sharey="row")
    for column, (name, _) in enumerate(looks):
        values = np.genfromtxt(directory / name / "gray.csv", delimiter=",", names=True,
                              dtype=None, encoding="utf-8")
        for row, control in enumerate(("H", "M", "S")):
            data = values[values["control"] == control]
            baseline = data[data["level"] == 0]
            base = np.stack([baseline[f"post_{c}"] for c in "rgb"], axis=-1) @ WEIGHTS * 255
            for level, color in zip(range(-5, 6), plt.cm.coolwarm(np.linspace(0, 1, 11))):
                v = data[data["level"] == level]
                code = np.stack([v[f"post_{c}"] for c in "rgb"], axis=-1) @ WEIGHTS * 255
                axes[row, column].plot(np.arange(256), code - base, label=f"{level:+d}",
                                       color=color, linewidth=1.2)
            ax = axes[row, column]
            ax.axhline(0, color="gray", linewidth=.5)
            ax.set_title(f"{name} / {control}")
            ax.grid(alpha=.2)
            if column == 0: ax.set_ylabel("Output change (8-bit code)")
    for ax in axes[-1]: ax.set_xlabel("Input sRGB code (0..255)")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=11)
    fig.suptitle("Deployed curve stops: output difference from level 0; one control at a time")
    fig.tight_layout(rect=(0, .04, 1, .96))
    fig.savefig(directory / "curve_stops.png", dpi=160)
    fig.savefig(directory / "curve_stops.svg")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frozen", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--jpeg-include", type=Path)
    parser.add_argument("--jpeg-lib", type=Path)
    parser.add_argument("--plot-only", action="store_true", help="redraw figures from existing measured CSV, without rerunning the C probe")
    args = parser.parse_args()
    looks = [("Neutral", None), ("Portra", "00A Portra 400 Endura"),
             ("Gold", "00B Gold 200 Endura"), ("ClassicChrome", "01 Classic Chrome")]
    if args.plot_only:
        plot_curves(args.out, looks)
        return
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((args.frozen / "package.json").read_text())
    for name, expected in manifest["files"].items():
        if sha(args.frozen / name) != expected:
            raise ValueError(f"frozen source changed: {name}")
    executable = args.out / "curve-probe"
    sources = [str(p) for directory in ("camera", "color", "image", "storage", "ui", "debug")
               for p in sorted((args.frozen / "source" / directory).glob("*.c"))]
    command = [args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-D_POSIX_C_SOURCE=200809L",
               "-I" + str(args.frozen / "include")]
    if args.jpeg_include: command += ["-I" + str(args.jpeg_include)]
    if args.jpeg_lib: command += ["-L" + str(args.jpeg_lib)]
    command += sources + [str(ROOT / "tests/curve_stops_probe.c"), "-ljpeg", "-lm", "-o", str(executable)]
    subprocess.run(command, check=True)
    report = dict(source=str(args.source.resolve()), source_sha256=sha(args.source),
                  frozen_build=json.loads((args.frozen / "build.json").read_text()),
                  compile_command=command, platform=platform.platform(),
                  limits="offline frozen core; historical local photo, not live camera; no camera RGB matrix, grain, resize, sharpening, legacy trim or JPEG loss",
                  looks={})
    summary_rows, samples = [], []
    for column, (name, stem) in enumerate(looks):
        out = args.out / name
        out.mkdir()
        cube = args.frozen / "romfs/luts" / (stem + ".cube") if stem else None
        subprocess.run([str(executable), str(out), str(cube) if cube else "-",
                        str(cube.with_suffix(".f3p")) if cube else "-", str(args.source)], check=True)
        values = np.genfromtxt(out / "gray.csv", delimiter=",", names=True, dtype=None, encoding="utf-8")
        raw = Image.open(out / "input.ppm").convert("RGB")
        raw.save(out / "input.png")
        (out / "input.ppm").unlink()
        raw_luma = luma(raw)
        image_bands = {"all": np.ones(raw_luma.shape, dtype=bool), "shadow_<64": raw_luma < 64,
                       "mid_64_192": (raw_luma >= 64) & (raw_luma < 192), "high_>=192": raw_luma >= 192,
                       "very_high_>=240": raw_luma >= 240}
        report["input_histogram"] = {k: dict(pixels=int(mask.sum()), percent=float(mask.mean() * 100))
                                      for k, mask in image_bands.items()}
        look_results = dict(cube_sha256=sha(cube) if cube else None, controls={})
        sheet = Image.new("RGB", (176 * 11, 160 * 3), (24, 24, 24))
        draw = ImageDraw.Draw(sheet)
        for row, control in enumerate(("H", "M", "S")):
            data = values[values["control"] == control]
            curves = {}
            for level in range(-5, 6):
                v = data[data["level"] == level]
                rgb = np.stack([v["post_r"], v["post_g"], v["post_b"]], axis=-1)
                curves[level] = dict(float_code=rgb @ WEIGHTS * 255,
                                     rgb8=np.floor(np.clip(rgb, 0, 1) * 255 + .5).astype(np.uint8),
                                     linear_y=v["post_linear_y"])
            neutral = curves[0]["float_code"]
            codes = np.arange(256)
            band = ((codes >= 192) & (codes <= 254)) if control == "H" else \
                   ((codes >= 1) & (codes < 64)) if control == "S" else ((codes >= 64) & (codes < 192))
            adjacent = np.stack([curves[i + 1]["float_code"] - curves[i]["float_code"] for i in range(-5, 5)])
            q = np.stack([curves[i]["rgb8"] for i in range(-5, 6)])
            distinct = np.array([len({tuple(v) for v in q[:, c]}) for c in range(256)])
            direction = -1 if control == "S" else 1
            control_stats = dict(
                peak_span_codes=float(np.max(np.abs(curves[5]["float_code"] - curves[-5]["float_code"]))),
                mean_abs_adjacent_step_in_band=float(np.mean(np.abs(adjacent[:, band]))),
                pct_adjacent_float_steps_under_one_code=float(np.mean(np.abs(adjacent[:, band]) < 1) * 100),
                pct_adjacent_exact_same_rgb8=float(np.mean(np.all(q[1:, band] == q[:-1, band], axis=-1)) * 100),
                mean_distinct_quantized_levels_in_band=float(distinct[band].mean()),
                direction_violations=int(np.sum(direction * adjacent[:, band] < -1e-4)),
                direction_violations_all_codes=int(np.sum(direction * adjacent < -1e-4)),
                spatial_monotonicity_violations=int(sum(np.sum(np.diff(curves[i]["linear_y"]) < -1e-6) for i in range(-5, 6))),
            )
            look_results["controls"][control] = control_stats
            for code in (0, 8, 16, 32, 56, 64, 96, 118, 128, 160, 192, 211, 224, 240, 248, 252, 254, 255):
                for level in range(-5, 6):
                    samples.append(dict(look=name, control=control, input_code=code, level=level,
                                        output_float_code=float(curves[level]["float_code"][code]),
                                        output_rgb8="/".join(map(str, curves[level]["rgb8"][code])),
                                        distinct_levels=int(distinct[code])))
            baseline = np.asarray(Image.open(out / f"{control}_+0.ppm"), dtype=float)
            baseline_y = baseline @ WEIGHTS
            previous = None
            for level in range(-5, 6):
                path = out / f"{control}_{level:+d}.ppm"
                with Image.open(path) as decoded:
                    image = decoded.convert("RGB")
                pixels = np.asarray(image, dtype=float)
                y = pixels @ WEIGHTS
                delta = y - baseline_y
                for band_name, mask in image_bands.items():
                    count = int(mask.sum())
                    summary_rows.append(dict(look=name, control=control, level=level, band=band_name,
                        pixels=count, mean_signed_code_delta=float(delta[mask].mean()) if count else None,
                        mean_abs_code_delta=float(np.abs(delta[mask]).mean()) if count else None,
                        max_abs_code_delta=float(np.max(np.abs(delta[mask]))) if count else None,
                        changed_rgb_percent=float(np.mean(np.any(pixels[mask] != baseline[mask], axis=-1))*100) if count else None,
                        same_as_previous_level_percent=float(np.mean(np.all(pixels[mask] == previous[mask], axis=-1))*100)
                            if count and previous is not None else None))
                previous = pixels
                image.save(path.with_suffix(".png"))
                path.unlink()
                x, top = (level + 5) * 176, row * 160
                draw.text((x + 4, top + 3), f"{control} {level:+d}", fill=(240, 240, 240))
                sheet.paste(image.resize((176, 132), Image.Resampling.LANCZOS), (x, top + 22))
            print(name, control, json.dumps(control_stats), flush=True)
        sheet.save(out / "photo_stops.png")
        report["looks"][name] = look_results
    plot_curves(args.out, looks)
    for name, rows in (("photo_metrics.csv", summary_rows), ("gray_samples.csv", samples)):
        with (args.out / name).open("w") as out:
            writer = csv.DictWriter(out, fieldnames=rows[0].keys())
            writer.writeheader(); writer.writerows(rows)
    (args.out / "results.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
