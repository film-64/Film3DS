"""Production contract/header synchronization and frozen UI package rejection."""
from pathlib import Path
import json
import hashlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from camera_runtime_contract import CONTRACT_MATRIX, RUNTIME_INTERNAL_FIELDS, header
from camera_ui15_package import checked_manifest, deploy, stage, prepare_deployment

class UiPackageTests(unittest.TestCase):
    def test_raw_only_shutter_and_existing_recovery(self):
        shutter=CONTRACT_MATRIX["film-camera-shutter-raw/v1"]
        self.assertTrue(shutter["compute"])
        self.assertFalse(shutter["raw_decode"])
        self.assertFalse(shutter["lane_admission"] or shutter["native_retention"])
        self.assertIsNone(shutter["override"])
        self.assertNotIn("film-roll-native-raw-pair/v1",CONTRACT_MATRIX)
        self.assertNotIn("film-camera-shutter-pair/v1",CONTRACT_MATRIX)
        evidence=CONTRACT_MATRIX["film-app-frozen-evidence/v1"]
        self.assertFalse(evidence["compute"] or evidence["camera_work"] or evidence["queues"])
        self.assertEqual(evidence["host"],"unchanged_retrieve_probe_ram.py")
        folder=ROOT/"include/diagnostics/existing_ram"
        for name,origin in json.loads((folder/"provenance.json").read_text()).items():
            self.assertEqual(hashlib.sha256((folder/name).read_bytes()).hexdigest(),origin["sha256"])

    def test_native_camera_boundary(self):
        preview = CONTRACT_MATRIX["film-camera-native-preview/v1"]
        exposure = CONTRACT_MATRIX["film-camera-native-exposure/v1"]
        capture = CONTRACT_MATRIX["film-camera-native-capture/v1"]
        self.assertTrue(all(p["compute"] and not p["lane_admission"] and p["override"] is None
                            for p in (preview, exposure, capture)))
        self.assertFalse(preview["register_gate"] or preview["startup_seed_capture"])
        self.assertEqual(preview["register_reads_per_frame"], 0)
        self.assertEqual(exposure["backend"], "existing_CAMU_SetExposure")
        self.assertEqual((exposure["minimum"], exposure["maximum"]), (-5, 5))
        self.assertFalse(exposure["independent_register_gain"] or exposure["calibrated_ev"])
        self.assertFalse(capture["development_effects"])
        manual = CONTRACT_MATRIX["film-camera-manual-setup/v1"]
        self.assertEqual(manual["owner"], "manual_UI_or_software_AE_exclusive")
        self.assertEqual(manual["setup_failure"], "surface_error_keep_native_preview_block_capture")

    def test_explicit_readiness_after_connect_failure(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
            receipt = Path(tmp) / "deployment.json"
            record = dict(sha256="same-binary", source_manifest_sha256="same-manifest")
            previous = dict(record, status="failed", failure_phase="connect", transfer_started=False)
            receipt.write_text(json.dumps(previous))
            with self.assertRaises(FileExistsError):
                prepare_deployment(receipt, record.copy(), None)
            for changed in (dict(status="started"), dict(status="NetLoader_payload_accepted_launch_requested"),
                            dict(failure_phase="payload"), dict(transfer_started=True), dict(sha256="changed")):
                receipt.write_text(json.dumps(dict(previous, **changed)))
                with self.assertRaises(FileExistsError):
                    prepare_deployment(receipt, record.copy(), "user reports NetLoader ready")
            receipt.write_text(json.dumps(previous))
            updated = record.copy()
            prepare_deployment(receipt, updated, "user reports NetLoader ready")
            self.assertEqual(updated["readiness_note"], "user reports NetLoader ready")
            self.assertEqual(json.loads((Path(tmp) / "deployment-failed-01.json").read_text()), previous)

    def test_receiver_restart_is_distinct_and_requires_payload_failure_and_same_binary(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
            receipt=Path(tmp)/"deployment.json"
            record=dict(sha256="same",source_manifest_sha256="same-manifest")
            previous=dict(record,status="failed",failure_phase="payload",transfer_started=True)
            receipt.write_text(json.dumps(previous))
            with self.assertRaises(FileExistsError):prepare_deployment(receipt,record.copy(),"merely ready")
            for change in (dict(status="started"),dict(failure_phase="launch"),dict(transfer_started=False),dict(sha256="different"),dict(source_manifest_sha256="different")):
                receipt.write_text(json.dumps(dict(previous,**change)))
                with self.assertRaises(FileExistsError):prepare_deployment(receipt,record.copy(),None,"user confirms receiver restarted")
            receipt.write_text(json.dumps(previous));updated=record.copy()
            prepare_deployment(receipt,updated,None,"user confirms receiver restarted")
            self.assertEqual(updated['source'],'film-camera-ui-receiver-reset-deploy/v1')
            self.assertEqual(updated['receiver_reset_note'],'user confirms receiver restarted')
            self.assertEqual(json.loads((Path(tmp)/'deployment-failed-01.json').read_text()),previous)
            self.assertEqual(len(updated['prior_failure_sha256']),64)

    def test_contract_and_portable_recipe(self):
        self.assertEqual((ROOT / "include/film_runtime_contract.h").read_text(), header())
        self.assertTrue(all(not p["lane_admission"] and p["override"] is None for p in CONTRACT_MATRIX.values()))
        self.assertEqual(CONTRACT_MATRIX["film-camera-exposure/v1"]["minimum"], -3)
        self.assertEqual(CONTRACT_MATRIX["film-camera-exposure/v1"]["maximum"], 3)
        self.assertEqual(CONTRACT_MATRIX["film-darkroom/v1"]["curve_levels"], tuple(range(-5, 6)))
        preview = CONTRACT_MATRIX["film-camera-preview/v1"]
        self.assertEqual(preview["register_service"], "cam:c")
        self.assertEqual(preview["chip_address"], 0x0000)
        self.assertEqual(preview["native_seed_frames"], 20)
        self.assertTrue(preview["native_seed_ae"] and preview["native_seed_awb"])
        # Consume the central registry, never a second denylist. Runtime state
        # must not become a field of the portable recipe/file structure.
        film = (ROOT / "include/film.h").read_text()
        for name in ("FilmRecipe", "FilmFrame"):
            body = re.search(r"typedef struct(?: \w+)? \{([^{}]*)\} " + name + ";", film).group(1)
            self.assertTrue(set(re.findall(r"\b\w+\b", body)).isdisjoint(RUNTIME_INTERNAL_FIELDS))

    def test_darkroom_preview_and_save_boundaries(self):
        develop = CONTRACT_MATRIX["film-darkroom/v1"]
        save = CONTRACT_MATRIX["film-darkroom-save/v1"]
        self.assertTrue(develop["compute"])
        self.assertTrue(save["compute"])
        self.assertFalse(develop["camera_calls"] or save["camera_calls"])
        self.assertFalse(develop["preview_writes"] or save["cancel_writes"] or save["recompute"])
        self.assertEqual(develop["look_switch"], "asset_only_preserve_current_adjustments")
        self.assertEqual(develop["exposure"], "linear_light_before_LUT")
        self.assertEqual(develop["user_tone"], "after_LUT_and_legacy_tone_before_HSL_and_saturation")
        self.assertEqual(develop["curve_model"], "monotone_cubic_offset_blend_in_sRGB_luma")
        self.assertEqual(develop["curve_zero"], "exact_bypass")
        self.assertEqual(develop["recipe_version"], 6)
        self.assertEqual(save["prerequisite"], "preview_then_SELECT_only")
        self.assertEqual(save["cancel"], "any_other_key_continues_normal_action")
        self.assertEqual(save["input"], "cached_displayed_RGB24_preview_for_RAW_and_native")
        self.assertEqual(save["writes"], "new_jpeg_only")
        self.assertEqual(save["retries"], 0)
        wb = CONTRACT_MATRIX["film-darkroom-wb-preview/v1"]
        self.assertTrue(wb["compute"])
        self.assertFalse(wb["writes"] or wb["camera_calls"])
        self.assertEqual(wb["input"], "original_or_cached_linear_scene_on_explicit_A_not_previous_preview")
        self.assertEqual(develop["percent_step"], 1)
        self.assertEqual((develop["dpad_step"], develop["circle_step"]), (1, 10))
        self.assertFalse(develop["acceleration"])
        self.assertEqual(develop["aspect_change"], "reset_centered_maximum")
        self.assertEqual(save["compute_scope"], "JPEG_encoding_only_no_original_read_no_ISP_LUT_resize_sharpen_or_grain")


    def test_on_device_darkroom_and_save_dispatch(self):
        text=(ROOT/"source/platform/main_3ds.c").read_text()
        wb=text.split("static void white_balance_input",1)[1].split("static void save_darkroom_preview",1)[0]
        self.assertIn("preview_darkroom(false,&trial)",wb)
        self.assertIn("preview_darkroom(false,NULL)",wb)
        self.assertIn("else if (keys&KEY_A)",wb)
        self.assertIn("FilmRecipe trial=app.darkroom",wb)
        save=text.split("static void save_darkroom_preview",1)[1].split("static void y2r_check",1)[0]
        self.assertNotIn("open_selected_original",save)
        self.assertNotIn("film_raw_export_pair",save)
        self.assertIn("if(app.darkroom_preview_fast)",save)
        self.assertIn("preview_darkroom(true,NULL)",save)
        finish=text.split("static bool finish_capture",1)[1].split("static bool capture_photo",1)[0]
        self.assertIn("bool saved = save->result.written;",finish)
        storage=CONTRACT_MATRIX["film-roll-capture-save/v1"]
        self.assertTrue(storage["compute"])
        self.assertFalse(storage["lane_admission"] or storage["retries"])
        self.assertIsNone(storage["override"])

    def test_composition_frame_survives_A_and_geometry_edits(self):
        text=(ROOT/"source/platform/main_3ds.c").read_text()
        draw=text.split("static void draw_composition",1)[1].split("/* ROLL_DRAW_BEGIN */",1)[0]
        self.assertNotIn("darkroom_preview_displayed",draw)
        self.assertNotIn("room.preview",draw)
        self.assertIn("film_composition_rect(app.source_width,app.source_height",draw)
        preview=text.split("static void preview_darkroom",1)[1].split("static void white_balance_input",1)[0]
        self.assertIn("show_image(&app.room.composition_view",preview)
        pan=text.split("/* DARKROOM_CONTROLS_BEGIN */",1)[1].split("/* DARKROOM_CONTROLS_END */",1)[0]
        self.assertNotIn("film_darkroom_discard(&app.room);",pan.split("} else {",1)[0])
        self.assertEqual(CONTRACT_MATRIX["film-darkroom-save/v1"]["display"],
            "full_original_composition_view_with_always_visible_original_coordinate_frame")

    def test_roll_boundaries(self):
        preview = CONTRACT_MATRIX["film-roll-preview/v1"]
        delete = CONTRACT_MATRIX["film-roll-delete/v1"]
        self.assertTrue(preview["compute"])
        self.assertFalse(preview["writes"] or preview["camera_calls"] or preview["original_reads"] or preview["ISP"])
        self.assertTrue(preview["thumbnails"])
        self.assertTrue(preview["selected_thumbnail"])
        raw=CONTRACT_MATRIX[preview["raw_preview"]]
        self.assertTrue(raw["compute"])
        self.assertFalse(raw["lane_admission"] or raw["original_reads"] or raw["ISP"] or raw["writes"] or raw["retries"])
        self.assertIsNone(raw["override"])
        self.assertEqual(raw["max_visible"],30)
        self.assertEqual(preview["grid_columns"], 10)
        self.assertEqual(preview["empty"], "zero_original_files_no_virtual_entries")
        self.assertFalse(delete["compute"])
        self.assertEqual(delete["entry"], "Y_then_A_only")
        self.assertEqual(delete["target"], "snapshotted_selected_DNG_or_legacy_F3S_ID_verified_before_unlink")
        self.assertEqual(delete["writes"], "selected_preview_JPG_F3I_and_F3C_then_DNG_and_legacy_duplicate_preserve_exports_missing_sidecars_OK")
        self.assertEqual(delete["retries"], 0)
        self.assertEqual(delete["path"],"exact_one_separator_after_directory_including_sdmc_trailing_slash")

    def test_native_manual_and_darkroom_only_denoise_contracts(self):
        mode=CONTRACT_MATRIX['film-camera-exposure-mode/v1']
        self.assertEqual(mode['modes'],('native_AE','manual'))
        self.assertFalse(mode['software_AE'])
        raw=CONTRACT_MATRIX['film-camera-raw-capture/v1']
        self.assertFalse(raw['denoise'])
        self.assertEqual(raw['native_seed_frames'],0)
        self.assertEqual(raw['exposure'],'selected_native_AE_snapshot_or_verified_live_manual_values_never_fixed128')
        self.assertNotIn('optional_chroma_median',CONTRACT_MATRIX['film-raw-isp/v1']['stages'])
        for name in ('film-camera-exposure-mode/v1','film-darkroom-denoise/v1','film-roll-pair-publish/v1','film-roll-original-open/v1','film-preview-catalog-prepare/v1'):
            rule=CONTRACT_MATRIX[name];self.assertTrue(rule['compute']);self.assertFalse(rule['lane_admission']);self.assertIsNone(rule['override'])

    def test_deferred_decode_and_linear_cache_dispatch(self):
        text=(ROOT/"source/platform/main_3ds.c").read_text()
        startup=text.split("    load_roll();",1)[1].split("    while (aptMainLoop())",1)[0]
        self.assertNotIn("film_roll_prepare_catalog",startup)
        save=(ROOT/"source/storage/roll_preview.c").read_text().split("FilmStatus film_roll_capture_save",1)[1]
        self.assertIn("film_dng_write",save)
        self.assertIn("film_frame_read",save)
        self.assertNotIn("film_roll_pair_publish",save)
        self.assertNotIn("film_develop",save)
        preview=text.split("static void preview_darkroom",1)[1].split("static void white_balance_input",1)[0]
        self.assertIn("film_darkroom_scene_preview",preview)
        self.assertIn("app.decoded_calibration!=isp_key",preview)
        self.assertIn("unsigned isp_key=app.raw_calibration?1u:0u;",preview)
        self.assertIn("film_raw_scene_preview_half",preview)
        self.assertIn("if(lut->has_transfer)",preview)
        self.assertNotIn("film_lut_free(&lut);",preview)
        grid=text.split("static bool roll_prepare_grid",1)[1].split("static void roll_show_selection",1)[0]
        self.assertIn("break;",grid)
        for name in ("film-roll-decode/v1","film-roll-decode-load/v1","film-darkroom-scene-preview/v1"):
            rule=CONTRACT_MATRIX[name]
            self.assertTrue(rule["compute"])
            self.assertFalse(rule["lane_admission"] or rule["retries"])
            self.assertIsNone(rule["override"])
        cache=(ROOT/"source/storage/roll_decode.c").read_text().split("FilmStatus film_roll_decode_load",1)[1]
        self.assertNotIn("film_frame_read",cache)
        self.assertNotIn("film_raw_frame_scene",cache)

    def test_shutter_worker_debug_and_standard_original_contracts(self):
        worker=CONTRACT_MATRIX["film-camera-shutter-worker/v1"]
        self.assertTrue(worker["compute"])
        self.assertEqual(worker["workers"],1)
        self.assertFalse(worker["lane_admission"] or worker["queues"] or worker["retries"])
        self.assertIsNone(worker["override"])
        text=(ROOT/"source/platform/main_3ds.c").read_text()
        shutter=text.split("static bool capture_photo",1)[1].split("static void draw",1)[0]
        self.assertNotIn("film_camera_capture_pair",shutter)
        self.assertIn("film_camera_raw_shutter_check",shutter)
        self.assertNotIn("film_mock_capture",shutter)
        self.assertNotIn("preview_snapshot",shutter)
        self.assertNotIn("film_camera_capture_diagnostic",shutter)
        worker=text.split("static void save_capture",1)[1].split("static bool finish_capture",1)[0]
        self.assertIn("film_camera_capture_raw",worker)
        self.assertNotIn("native_capture",worker)
        self.assertNotIn("F3S",worker)
        self.assertIn('ROLL%04u.DNG',worker)
        self.assertNotIn("save_capture(save)",shutter)
        self.assertIn("CAPTURE_ACQUIRING",text)
        end=text.rsplit("    film_darkroom_discard(&app.room);",1)[1]
        self.assertLess(end.index("finish_capture(true)"),end.index("film_camera_close"))
        self.assertIn('LOGS ROOT "log/"',text)
        self.assertIn('text_at(350,222,.45f,gold,"debug")',text)
        self.assertNotIn("load_look_profile",text)
        raw=(ROOT/"source/platform/raw_camera_3ds.c").read_text()
        manual=(ROOT/"source/platform/manual_exposure_3ds.c").read_text()
        self.assertIn("film_camera_read_bool",raw)
        self.assertIn("if(R_FAILED(rc)||ae)return FILM_STATE",manual)
        self.assertIn("standard_RawImageDigest",CONTRACT_MATRIX["film-dng-read/v1"]["integrity"])

    def test_home_close_cleanup_does_not_wait_for_another_vblank(self):
        rule=CONTRACT_MATRIX["film-app-shutdown/v1"]
        self.assertFalse(rule["compute"] or rule["lane_admission"] or rule["vblank_wait"] or rule["new_frames"])
        self.assertIsNone(rule["override"])
        text=(ROOT/"source/platform/main_3ds.c").read_text()
        cleanup=text.rsplit("\ncleanup:",1)[1]
        for wait in ("C3D_FrameSync(","gspWaitForVBlank(","C3D_FrameBegin("):
            self.assertNotIn(wait,cleanup)
        drain=cleanup.index("C3D_Fini();")
        for release in ("C3D_TexDelete(","C2D_TextBufDelete(","C2D_Fini();","gfxExit();"):
            self.assertLess(drain,cleanup.index(release))
        self.assertEqual(cleanup.count("C3D_Fini();"),1)
        self.assertLess(cleanup.index("C2D_Fini();"),cleanup.index("gfxExit();"))
        shutdown=text.rsplit("    film_darkroom_discard(&app.room);",1)[1]
        self.assertLess(shutdown.index("finish_capture(true)"),shutdown.index("film_camera_close"))
        self.assertLess(shutdown.index("film_camera_close"),shutdown.index("\ncleanup:"))

    def test_gray_v2_trial_and_both_grain_backend_display_fixes(self):
        main=(ROOT/"source/platform/main_3ds.c").read_text()
        gpu=(ROOT/"source/platform/grain_gpu_3ds.c").read_text()
        geometry=(ROOT/"include/preview_texture.h").read_text()
        layout=(ROOT/"include/gpu_layout.h").read_text()
        profile=(ROOT/"romfs/calibration/display-gray-v2.f3curve").read_text()
        original=(ROOT/"romfs/calibration/display-gray.f3isp").read_text()
        self.assertTrue(profile.startswith("F3GRAYCURVE1\n"))
        self.assertTrue(original.startswith("F3ISP1\n"))
        self.assertIn('const char *path="romfs:/calibration/display-gray.f3isp";',main)
        self.assertNotIn('"Display color":app.raw_calibration',main)
        self.assertIn('app.raw_calibration==2&&app.roll_preview_format==FILM_RAW10_RGGB',main)
        self.assertIn('film_gray_curve_read(curve_file,&curve)',main)
        self.assertIn('"Gray original"',main)
        self.assertIn('"Gray v2 (trial)"',main)
        self.assertIn('film_preview_texture_dims(image->width,image->height,&tex_w,&tex_h)',main)
        self.assertIn('film_preview_texture_dims(app.image_width,app.image_height,&tw,&th)',main)
        self.assertIn('sub.right=tw/(float)PREVIEW_TEX_SIDE',main)
        self.assertIn('sub.bottom=1-th/(float)PREVIEW_TEX_SIDE',main)
        self.assertIn("film_gpu_readback_axes(gpu_workspace.readback,SIDE,&axes)",gpu)
        self.assertIn("film_gpu_readback_pixel(gpu_workspace.readback,SIDE,x,y,axes)",gpu)
        self.assertIn("FilmGpuReadbackAxes",layout)
        self.assertIn("film_preview_texture_dims",geometry)
        self.assertNotIn("bool flip=top[2]",gpu)

    def test_gpu_final_composition_reuses_resources_and_cache(self):
        gpu=(ROOT/"source/platform/grain_gpu_3ds.c").read_text()
        self.assertNotIn("gpu_mix(",gpu)
        self.assertNotIn("float *noise",gpu)
        self.assertNotIn("memcpy(im->pixels",gpu)
        self.assertEqual(gpu.count("C3D_SyncDisplayTransfer("),1)
        self.assertIn("C3D_RenderTargetCreateFromTex",gpu)
        self.assertIn("GPU_L8",gpu)
        tile=gpu.split("static FilmStatus render_tile",1)[1].split("FilmStatus film_3ds_grain_layered",1)[0]
        self.assertNotIn("C3D_TexInit",tile)
        self.assertNotIn("malloc(",tile)
        self.assertIn("no CPU blend",tile)
        room=(ROOT/"source/ui/darkroom.c").read_text().split("static FilmStatus render_preview",1)[1].split("FilmStatus film_darkroom_preview",1)[0]
        self.assertEqual(room.count("film_develop_canvas_render_pair("),1)
        self.assertNotIn("film_develop_canvas_render(",room)
        main=(ROOT/"source/platform/main_3ds.c").read_text()
        self.assertIn(".output_renderer=app.grain_gpu?film_3ds_canvas_output:NULL",main)
        cleanup=main.rsplit("\ncleanup:",1)[1]
        self.assertLess(cleanup.index("film_3ds_develop_release()"),cleanup.index("C3D_Fini()"))
        for name in ("film-grain-gpu-compose/v1","film-darkroom-gpu-output/v1","film-darkroom-render-pair/v1","film-raw-bad-pixel/v1"):
            rule=CONTRACT_MATRIX[name];self.assertTrue(rule["compute"])
            self.assertFalse(rule["lane_admission"] or rule["retries"]);self.assertIsNone(rule["override"])

    def test_prepared_shutter_and_decode_only_roll(self):
        rule=CONTRACT_MATRIX["film-camera-raw-capture/v1"]
        self.assertEqual((rule["manual_gate_frames"],rule["raw_settle_frames"],rule["raw_retained_frames"],rule["restored_frames"]),(1,0,1,1))
        raw=(ROOT/"source/platform/raw_camera_3ds.c").read_text()
        capture=raw.split("FilmStatus film_camera_capture_raw",1)[1]
        self.assertNotIn("film_camera_prepare_raw(",capture)
        self.assertNotIn("film_camera_verify_manual_capture(",capture)
        self.assertIn("film_raw_capture_transaction_result",capture)
        preview=(ROOT/"source/platform/camera_3ds.c").read_text().split("FilmStatus film_camera_preview_poll",1)[1]
        self.assertIn("!cam->raw_preparation.attempted",preview)
        self.assertIn("film_camera_prepare_raw",preview)
        main=(ROOT/"source/platform/main_3ds.c").read_text()
        self.assertIn("app.ui.page == FILM_VIEW && !capture_requested",main)
        publisher=(ROOT/"source/storage/roll_preview.c").read_text()
        for name in ("film_develop", "film_raw_scene_grade", "film_resize"):
            self.assertNotIn(name,publisher)
        self.assertNotIn("film_roll_scene_publish",publisher)
        prepare=CONTRACT_MATRIX["film-camera-raw-prepare/v1"]
        self.assertTrue(prepare["compute"]);self.assertEqual(prepare["camera_frames"],0)
        self.assertFalse(prepare["lane_admission"] or prepare["retries"])
        self.assertIsNone(prepare["override"])

    def test_roll_to_darkroom_uses_the_resident_scene(self):
        text=(ROOT/"source/platform/main_3ds.c").read_text()
        sync=text.split("static void sync_darkroom",1)[1].split("static void change_darkroom_look",1)[0]
        hot=sync.split("if(app.decoded_id==id&&app.decoded_scene.pixels){",1)[1].split("}else",1)[0]
        self.assertIn("frame.recipe=app.decoded_recipe",hot)
        self.assertNotIn("film_roll_decode_load",hot)
        self.assertNotIn("film_raw_scene_free",hot)
        step=text.split("static void decode_roll_step",1)[1].split("static void sync_darkroom",1)[0]
        self.assertIn("selected?&app.decoded_scene:NULL",step)
        refresh=text.split("/* ROLL_REFRESH_BEGIN */",1)[1].split("/* ROLL_REFRESH_END */",1)[0]
        self.assertNotIn("app.roll_preview_id=0",refresh)
        preview=text.split("static void preview_darkroom",1)[1].split("static void white_balance_input",1)[0]
        self.assertIn("app.decoded_calibration!=isp_key",preview)
        self.assertIn("unsigned isp_key=app.raw_calibration?1u:0u;",preview)
        self.assertIn("film_raw_scene_preview_half",preview)
        self.assertIn("if(lut->has_transfer)",preview)
        self.assertNotIn("film_lut_free(&lut);",preview)
        self.assertIn("else if(app.roll_preview_format==FILM_RAW10_RGGB)status=FILM_STATE",preview)

    def test_debug_logging_never_clobbers_camera_ipc_buffer(self):
        text=(ROOT/"source/platform/camera_bool_3ds.c").read_text()
        prepare=text.index("u32 *b=getThreadCommandBuffer()")
        send=text.index("svcSendSyncRequest(camHandle)")
        copy=text.index("memcpy(reply,b,sizeof reply)")
        self.assertLess(text.index("fflush(log)"),prepare)
        self.assertNotIn("fprintf",text[prepare:send])
        self.assertNotIn("fflush",text[prepare:send])
        self.assertLess(send,copy)
        self.assertLess(copy,text.index('if(log){fprintf(log,"CAM bool cmd=%02x header='))

    def test_hardware_ae_window_and_absolute_level_reinit(self):
        main=(ROOT/"source/platform/main_3ds.c").read_text()
        native=(ROOT/"source/platform/camera_3ds.c").read_text()
        mode=(ROOT/"source/platform/manual_exposure_3ds.c").read_text()
        gray=(ROOT/"source/storage/gray_capture.c").read_text()
        roll=(ROOT/"source/storage/roll_decode.c").read_text()
        self.assertIn('CAMU_SetAutoExposureWindow(select',native)
        self.assertIn('cam->ae_window_applied=R_SUCCEEDED(rc)',native)
        self.assertIn('FilmStatus s=reopen_native_ae',main)
        self.assertIn('film_camera_open(&app.camera,FILM_YUYV,sensor)',main)
        self.assertIn('film_camera_set_ae_window(&app.camera,requested)',main)
        self.assertIn('cam->ae_window_applied=false',mode)
        self.assertIn('change_meter_region(region)',main)
        self.assertIn('app.camera.ae_window_applied',main)
        self.assertIn('put(b+60,p->meter_region.x)',gray)
        self.assertIn('film_roll_decode_last_stage',roll)
        self.assertIn('last_stage="F3C scene cache write"',roll)
        self.assertFalse(CONTRACT_MATRIX["film-camera-ae-window/v1"]["queues"])
        self.assertEqual(CONTRACT_MATRIX["film-camera-ae-window/v1"]["retries"],0)

    def test_11_step_native_and_quick_meter_ui_contract(self):
        camera=(ROOT/"include/platform_camera.h").read_text()
        source=(ROOT/"source/platform/main_3ds.c").read_text()
        controls=(ROOT/"source/platform/camera_3ds.c").read_text()
        gray=(ROOT/"source/storage/gray_capture.c").read_text()
        self.assertEqual(
            (CONTRACT_MATRIX["film-camera-native-exposure/v1"]["minimum"],
             CONTRACT_MATRIX["film-camera-native-exposure/v1"]["maximum"]),(-5,5))
        self.assertIn("FILM_MANUAL_EXPOSURE_MIN = FILM_HW_MIN",camera)
        self.assertIn("FILM_MANUAL_EXPOSURE_MAX = FILM_HW_MAX",camera)
        self.assertIn("FILM_MANUAL_EXPOSURE_MIN:FILM_EXPOSURE_MIN",controls)
        self.assertIn("FILM_MANUAL_EXPOSURE_MAX:FILM_EXPOSURE_MAX",controls)
        self.assertIn('p->native_level< -5||p->native_level>5',gray)
        self.assertIn('app.ui.page==FILM_VIEW && !(held&KEY_SELECT)',source)
        self.assertIn('adjust_camera((!!(keys&KEY_RIGHT))-(!!(keys&KEY_LEFT)))',source)
        self.assertIn('film_ae_region_resize(app.ae_region,',source)
        self.assertIn('film_ae_region_move(region,dx,dy)',source)
        self.assertIn("SEL+R PAIR",source)
        self.assertIn("A/R RAW",source)
        self.assertIn("NATIVE AE LEVEL",source)
        self.assertNotIn("for (unsigned i = 0; i < 3; ++i) C2D_DrawRectSolid(10, 57",source)

    def test_gray_pair_capture_and_source_contract(self):
        main=(ROOT/"source/platform/main_3ds.c").read_text()
        storage=(ROOT/"source/storage/gray_capture.c").read_text()
        decode=(ROOT/"source/storage/roll_decode.c").read_text()
        dng=(ROOT/"source/storage/dng.c").read_text()
        self.assertIn('capture_photo((keys&KEY_R)&&(held&KEY_SELECT))',main)
        self.assertIn('app.camera.preview_dma[app.camera.preview_pending^1]',main)
        self.assertIn('film_gray_pair_write("film-gray-pair-write/v1"',main)
        self.assertIn('film_gray_pair_verify("film-gray-pair-verify/v1"',main)
        self.assertIn('gray==FILM_IO',decode)
        self.assertIn('ROLL%04u.F3Y',storage)
        self.assertNotIn('film_gray_pair_',dng)
        self.assertFalse(CONTRACT_MATRIX["film-gray-pair-write/v1"]["compute"])
        self.assertFalse(CONTRACT_MATRIX["film-gray-pair-write/v1"]["queues"])
        self.assertEqual(CONTRACT_MATRIX["film-gray-pair-write/v1"]["retries"],0)

    def test_exposure_matrix_has_separate_neutral_and_shot_presets(self):
        root = ROOT
        main = (root/"source/platform/main_3ds.c").read_text()
        decode = (root/"source/storage/roll_decode.c").read_text()
        raw = (root/"source/image/raw_darkroom.c").read_text()
        dng = (root/"source/storage/dng.c").read_text()
        roll = (root/"source/ui/roll.c").read_text()
        self.assertIn('film_exposure_matrix_write("film-exposure-matrix-write/v1"',main)
        self.assertIn('film_exposure_matrix_fit("film-exposure-matrix-fit/v1"',decode)
        self.assertIn('film_exposure_matrix_read("film-exposure-matrix-read/v1"',main)
        self.assertIn('Shot (approx)',main)
        self.assertIn('Neutral',main)
        self.assertIn('app.shot_curve_mode=2',main)
        self.assertIn('app.darkroom = app.saved_recipe = frame.recipe;',main)
        self.assertIn('exp2f(recipe->input_tone.value[FILM_TONE_EV])',raw)
        self.assertIn('scene_gain(y,o)',raw)
        self.assertIn('FILM_RAW10_RGGB',dng)
        self.assertNotIn('film_exposure_matrix_',dng)
        self.assertIn('"F3E"',roll)
        for name in ("film-exposure-matrix-fit/v1",
                     "film-exposure-matrix-read/v1",
                     "film-exposure-matrix-write/v1"):
            self.assertIn(name, CONTRACT_MATRIX)

    def test_frozen_package(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
            directory = Path(tmp)
            luts = directory / "luts"
            luts.mkdir()
            cube = luts / "Test.cube"
            cube.write_text("LUT_3D_SIZE 17\n" + "0 0 0\n" * 17**3)
            self.assertFalse(cube.with_suffix(".f3p").exists())
            out = directory / "ui"
            stage(out, luts)
            manifest = checked_manifest(out)
            self.assertTrue(manifest["ui"])
            self.assertEqual(manifest["lut_count"], 1)
            self.assertFalse((out/"romfs/luts/Test.f3p").exists())
            self.assertEqual((out / "romfs/luts/Test.cube").read_bytes(), cube.read_bytes())
            with self.assertRaises(FileExistsError):
                stage(out, luts)
            # Missing build proof rejects before any network connection; no
            # permissive mock stands in for the deployment gate.
            with self.assertRaises(FileNotFoundError):
                deploy(out, "192.0.2.1", 17491)
            self.assertFalse((out / "deployment.json").exists())
            (out / "source/platform/main_3ds.c").write_text("changed")
            with self.assertRaises(ValueError):
                checked_manifest(out)

if __name__ == "__main__":
    unittest.main()
