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
    def test_shutter_native_jpeg_and_existing_recovery(self):
        shutter=CONTRACT_MATRIX["film-camera-shutter-pair/v1"]
        self.assertTrue(shutter["compute"])
        self.assertFalse(shutter["raw_decode"] or shutter["lane_admission"])
        self.assertIsNone(shutter["override"])
        self.assertFalse(CONTRACT_MATRIX["film-roll-native-raw-pair/v1"]["raw_decode"])
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
        self.assertEqual((exposure["minimum"], exposure["maximum"]), (-3, 3))
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
            body = re.search(r"typedef struct \{([^{}]*)\} " + name + ";", film).group(1)
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
        self.assertEqual(save["input"], "cached_full_original_RGB16_canvas")
        self.assertEqual(save["writes"], "new_jpeg_only")
        self.assertEqual(save["retries"], 0)
        wb = CONTRACT_MATRIX["film-darkroom-wb-preview/v1"]
        self.assertTrue(wb["compute"])
        self.assertFalse(wb["writes"] or wb["camera_calls"])
        self.assertEqual(wb["input"], "original_source_downsample_not_previous_preview")
        self.assertEqual(develop["percent_step"], 1)
        self.assertEqual((develop["dpad_step"], develop["circle_step"]), (1, 10))
        self.assertFalse(develop["acceleration"])
        self.assertEqual(develop["aspect_change"], "reset_centered_maximum")
        self.assertEqual(save["compute_scope"], "crop_resize_sharpen_grain_only_no_LUT_recompute")


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
        self.assertEqual(delete["target"], "snapshotted_selected_F3S_ID_and_index_verified_before_unlink")
        self.assertEqual(delete["writes"], "one_original_F3S_only_preserve_exported_JPEG")
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

    def test_frozen_package(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
            directory = Path(tmp)
            luts = directory / "luts"
            luts.mkdir()
            cube = luts / "Test.cube"
            cube.write_text("LUT_3D_SIZE 17\n" + "0 0 0\n" * 17**3)
            with self.assertRaises(ValueError):
                stage(directory / "missing-profile", luts)
            subprocess.run([str(ROOT / "build/filmctl"), "profile", str(cube.with_suffix(".f3p"))], check=True)
            out = directory / "ui"
            stage(out, luts)
            manifest = checked_manifest(out)
            self.assertTrue(manifest["ui"])
            self.assertEqual(manifest["lut_count"], 1)
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
