"""Freeze a darkroom-only patch on an already verified application package.

Preserves the deployed camera implementation while another task edits the live
camera tree. No deployment, camera access, or admission override is performed.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil

from camera_runtime_contract import CONTRACT_MATRIX, header
from camera_ui15_package import ROOT, checked_manifest, sha

OVERLAYS = ("include/film.h", "source/color/color.c", "source/image/image.c",
            "source/storage/storage.c", "source/ui/darkroom.c", "include/darkroom.h",
            "source/color/white_balance.c", "source/color/cie1931_5nm.h", "source/image/composition.c")


def stage_patch(base, out):
    policy = CONTRACT_MATRIX["film-camera-ui-package/v1"]
    assert policy["source_selection"] == "current_or_verified_base_plus_explicit_darkroom_patch"
    assert policy["patch_camera_policy"] == "preserve_base_camera_sources_and_headers"
    manifest = checked_manifest(base)
    built = json.loads((base / "build.json").read_text())
    if built != dict(app_sha256=sha(base / "Film3DS.3dsx"),
                     source_manifest_sha256=sha(base / "package.json"),
                     arm11_verified=True, romfs_verified=True):
        raise ValueError("base package lacks matching verified build receipt")
    if out.exists():
        raise FileExistsError(out)
    matrix = copy.deepcopy(manifest["runtime_contract"])
    for source in ("film-darkroom/v1", "film-darkroom-save/v1", "film-darkroom-wb-preview/v1", "film-camera-ui-package/v1"):
        matrix[source] = copy.deepcopy(CONTRACT_MATRIX[source])
    out.mkdir(parents=True)
    for directory in ("source", "include", "romfs", "tools"):
        shutil.copytree(base / directory, out / directory)
    for name in ("Makefile", "Makefile.3ds"):
        shutil.copy2(base / name, out / name)
    for name in OVERLAYS:
        shutil.copy2(ROOT / name, out / name)
    # Transplant only explicit darkroom functions and input blocks. Camera
    # initialization, service calls, capture and exposure retain the verified base.
    main = out / "source/platform/main_3ds.c"
    text = main.read_text()
    current = (ROOT / "source/platform/main_3ds.c").read_text()
    replacements = 0
    def section(value, start, end):
        a = value.index(start)
        return value[a:value.index(end, a)]
    for start, end in (("static void adjust_darkroom(", "static FilmStatus take("),
                       ("static void preview_darkroom(", "static void y2r_check("),
                       ("static void darkroom_value(", "static void draw(C3D_RenderTarget")):
        text = text.replace(section(text, start, end), section(current, start, end), 1)
        replacements += 1
    a = text.index("        /* Only SELECT alone saves.")
    b = text.index("        if (keys & KEY_START)", a)
    block = section(current, "        /* DARKROOM_PREVIEW_INPUT_BEGIN */", "        /* DARKROOM_PREVIEW_INPUT_END */")
    text = text[:a] + block + "        /* DARKROOM_PREVIEW_INPUT_END */\n" + text[b:]
    anchor = "        } else if (app.ui.page == FILM_DARKROOM) {\n"
    a = text.index(anchor, text.index("while (aptMainLoop())")) + len(anchor)
    b = text.index("        } else {", a)
    block = section(current, "            /* DARKROOM_CONTROLS_BEGIN */", "            /* DARKROOM_CONTROLS_END */")
    text = text[:a] + block + "            /* DARKROOM_CONTROLS_END */\n" + text[b:]
    text = text.replace("        if (keys & KEY_B) {", "        if ((keys & KEY_B) && app.ui.page!=FILM_DARKROOM) {", 1)
    text = text.replace("            preview_darkroom();", "            preview_darkroom(false);")
    anchor = "    C2D_DrawImageAt((C2D_Image){&app.texture, &sub}, x, y, 0, NULL, 1, 1);\n"
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + "    draw_composition(x,y,sub.width,sub.height,gold);\n")
    anchor = "    C2D_SceneBegin(bottom);\n"
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + "    if (app.ui.page==FILM_DARKROOM && app.room.wb_editor) {\n        draw_white_balance(bg,fg,gold);C3D_FrameEnd(0);return;\n    }\n")
    replacements += 6
    main.write_text(text)
    (out / "include/film_runtime_contract.h").write_text(header(matrix))
    jpeg = base / "build-3ds/jpeg-arm11/source"
    if jpeg.is_dir():
        shutil.copytree(jpeg, out / "build-3ds/jpeg-arm11/source")
    camera_files = [name for name in manifest["files"] if name.startswith("source/camera/") or
                    name in ("source/platform/camera_3ds.c", "source/platform/hardware_qa_3ds.c",
                             "include/platform_camera.h", "include/hardware_exposure.h", "include/hardware_transport.h")]
    for name in camera_files:
        if sha(out / name) != manifest["files"][name]:
            raise ValueError(f"camera baseline changed: {name}")
    hashes = {str(p.relative_to(out)): sha(p) for directory in ("source", "include", "romfs", "tools")
              for p in sorted((out / directory).rglob("*")) if p.is_file()}
    hashes.update({name: sha(out / name) for name in ("Makefile", "Makefile.3ds")})
    record = dict(source="film-camera-ui-package/v1", ui=True, lut_count=manifest["lut_count"],
                  files=hashes, runtime_contract=matrix,
                  darkroom_patch=dict(base_build_sha256=built["app_sha256"],
                      base_manifest_sha256=sha(base / "package.json"), overlays=list(OVERLAYS),
                      main_ui_replacements=replacements,
                      preserved_camera_files={name: manifest["files"][name] for name in camera_files},
                      staging_tool_sha256=sha(Path(__file__))))
    (out / "package.json").write_text(json.dumps(record, indent=2) + "\n")
    checked_manifest(out)
    print(f"Staged darkroom-only patch, {len(camera_files)} camera files unchanged: {out}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    stage_patch(args.base.resolve(), args.out.resolve())
