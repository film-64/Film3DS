"""Freeze only Roll UI/deletion changes over an already built and verified package."""
import argparse
import copy
import json
from pathlib import Path
import shutil
from camera_runtime_contract import CONTRACT_MATRIX, header
from camera_ui15_package import ROOT, checked_manifest, sha


def section(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def stage(base, out):
    assert CONTRACT_MATRIX["film-camera-ui-package/v1"]["roll_patch"] == "verified_base_plus_explicit_roll_patch_preserve_camera_and_darkroom"
    manifest = checked_manifest(base)
    built = json.loads((base / "build.json").read_text())
    if built != dict(app_sha256=sha(base / "Film3DS.3dsx"), source_manifest_sha256=sha(base / "package.json"),
                     arm11_verified=True, romfs_verified=True):
        raise ValueError("base package has no matching verified build")
    if out.exists():
        raise FileExistsError(out)
    out.mkdir(parents=True)
    for directory in ("source", "include", "romfs", "tools"):
        shutil.copytree(base / directory, out / directory)
    for name in ("Makefile", "Makefile.3ds"):
        shutil.copy2(base / name, out / name)
    overlays = ("include/roll.h", "source/ui/roll.c")
    for name in overlays:
        shutil.copy2(ROOT / name, out / name)
    main = out / "source/platform/main_3ds.c"
    text = main.read_text()
    current = (ROOT / "source/platform/main_3ds.c").read_text()
    text = text.replace('#include "darkroom.h"', '#include "darkroom.h"\n#include "roll.h"', 1)
    text = text.replace('    FilmDarkroom room;', '    FilmDarkroom room;\n    FilmRoll roll;', 1)
    text = text.replace(section(text, "static unsigned roll_group_start(", "static void load_roll("),
                        section(current, "/* ROLL_PREVIEW_BEGIN */", "static void load_roll("), 1)
    text = text.replace('app.ui = film_ui_new(app.count ? app.count : VIRTUAL_PHOTOS);', 'app.ui = film_ui_new(app.count);', 1)
    start = text.index("static void draw(C3D_RenderTarget")
    text = text[:start] + section(current, "/* ROLL_DRAW_BEGIN */", "static void draw(C3D_RenderTarget") + text[start:]
    image = '    C2D_DrawImageAt((C2D_Image){&app.texture, &sub}, x, y, 0, NULL, 1, 1);'
    assert text.count(image) == 1
    block = section(current, '    /* ROLL_EMPTY_IMAGE_BEGIN */', '    /* ROLL_EMPTY_IMAGE_END */') + '    /* ROLL_EMPTY_IMAGE_END */'
    text = text.replace(image, block, 1)
    text = text.replace('    if (app.ui.page != FILM_DARKROOM)\n        for', '    if (app.ui.page == FILM_VIEW)\n        for', 1)
    a = text.index('    } else if (app.ui.page == FILM_ROLL) {', text.index('static void draw(C3D_RenderTarget'))
    b = text.index('    } else draw_darkroom', a)
    text = text[:a] + '    } else if (app.ui.page == FILM_ROLL) {\n        draw_roll(bg,fg,gold,dim);\n' + text[b:]
    block = section(current, '        /* ROLL_MODAL_INPUT_BEGIN */', '        /* ROLL_MODAL_INPUT_END */') + '        /* ROLL_MODAL_INPUT_END */\n'
    text = text.replace('        /* DARKROOM_PREVIEW_INPUT_BEGIN */', block + '        /* DARKROOM_PREVIEW_INPUT_BEGIN */', 1)
    a = text.index('            if (app.ui.page == FILM_ROLL) {', text.index('while (aptMainLoop())'))
    b = text.index('            bool live_view', a)
    text = text[:a] + section(current, '            /* ROLL_CONTROLS_BEGIN */', '            bool live_view') + text[b:]
    block = section(current, '            /* ROLL_TOUCH_BEGIN */', '            if (app.ui.page == FILM_DARKROOM && touch.py >= FILM_DARKROOM_ROW_Y &&')
    text = text.replace('            if (app.ui.page == FILM_DARKROOM && touch.py >= FILM_DARKROOM_ROW_Y &&',
                        block + '            if (app.ui.page == FILM_DARKROOM && touch.py >= FILM_DARKROOM_ROW_Y &&', 1)
    a = text.index('        if ((saved || app.ui.page != previous_page || app.ui.selection != previous_photo)')
    b = text.index('        sync_darkroom();', a)
    text = text[:a] + section(current, '        /* ROLL_REFRESH_BEGIN */', '        sync_darkroom();\n        draw(top, bottom);') + text[b:]
    text = text.replace('        if (app.ui.page == FILM_ROLL) update_roll_thumbnails();\n', '')
    main.write_text(text)
    matrix = copy.deepcopy(manifest['runtime_contract'])
    for name in ('film-roll-preview/v1', 'film-roll-delete/v1', 'film-camera-ui-package/v1'):
        matrix[name] = copy.deepcopy(CONTRACT_MATRIX[name])
    (out / 'include/film_runtime_contract.h').write_text(header(matrix))
    changed = set(overlays) | {'source/platform/main_3ds.c', 'include/film_runtime_contract.h'}
    preserved = {name: value for name, value in manifest['files'].items() if name not in changed}
    for name, expected in preserved.items():
        if sha(out / name) != expected:
            raise ValueError(f'unrelated baseline changed: {name}')
    jpeg = base / 'build-3ds/jpeg-arm11/source'
    if jpeg.is_dir():
        shutil.copytree(jpeg, out / 'build-3ds/jpeg-arm11/source')
    files = {str(p.relative_to(out)): sha(p) for directory in ('source', 'include', 'romfs', 'tools')
             for p in sorted((out / directory).rglob('*')) if p.is_file()}
    files.update({name: sha(out / name) for name in ('Makefile', 'Makefile.3ds')})
    record = dict(source='film-camera-ui-package/v1', ui=True, lut_count=manifest['lut_count'],
                  files=files, runtime_contract=matrix,
                  roll_patch=dict(base_build_sha256=built['app_sha256'], overlays=sorted(changed),
                                  preserved_files=preserved, staging_tool_sha256=sha(Path(__file__))))
    (out / 'package.json').write_text(json.dumps(record, indent=2) + '\n')
    checked_manifest(out)
    print(f'Staged Roll patch, {len(preserved)} unrelated files unchanged: {out}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    stage(args.base.resolve(), args.out.resolve())
