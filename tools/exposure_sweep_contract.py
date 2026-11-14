"""Authoritative local diagnostic matrix. No Vision scheduler entry or admission.

The C header is generated from this file; unknown entries fail closed. Codes
are experimental settings, never calibrated multipliers. All evidence stays
in local diagnostic archives, never portable image metadata.
"""
from pathlib import Path

SOURCE = "exposure-sweep/v2"
CONTRACT_MATRIX = {
    SOURCE: dict(compute=True, lane_admission=False,
                 override="standalone_hardware_diagnostic", proof="local_trace",
                 sensor=1, fps=5, frame_lines=2114, max_coarse=1585,
                 settle=8, measured=4, measurement_retries=0,
                 storage="RAM", ram_bytes=28 * 1024**2, max_files=320,
                 raw="last_measured_center_128x96_YUYV_per_stage", ipc_dump=False,
                 recovery="frozen_RAM_v3_CRC_ranges"),
    "exposure-sweep-analysis/v1": dict(compute=False, lane_admission=False,
                 override=None, proof="local_trace", max_error_ev=.25,
                 max_reference_drift_ev=.125, max_stage_center_y_span=2.,
                 minimum_holdout_exposures=2, holdout_common_pixel_fraction=.8,
                 prediction_roi_coverage=1., minimum_transfer_separation_ev=1.,
                 inverse_response="monotonic_log_response_bounded_least_squares", samples=100,
                 smoothing=10., random=False, min_log_increment=1e-6, solver_iterations=200),
    "exposure-sweep-session/v1": dict(compute=True, lane_admission=False,
                 override="standalone_hardware_diagnostic", proof="local_trace",
                 deployments=1, measurement_retries=0, readiness_deadline_seconds=3600,
                 readiness_poll_seconds=2, recovery="frozen_RAM_v3_CRC_ranges",
                 console="bounded_status_only", detailed_log="session.log",
                 status="atomic_session_status_json", raw_evidence="local_files_only"),
}
COARSE = (4, 8, 16, 32, 64, 128, 256, 528, 1024, 1585)
ANALOG_CODES = (0x10, 0x18, 0x20, 0x28, 0x30, 0x3c)
CORE_CODES = (1, 2)
ISP_MODES = ("native", "unity")
# Full Cartesian product in the declared domain, plus original color ratios.
STATES = [("native", 0, "native")]
STATES += [(f"a{a:02x}_d{d}_{isp}", (d << 9) | a, isp)
           for isp in ISP_MODES for d in CORE_CODES for a in ANALOG_CODES]
MAX_STAGES = 3 + len(STATES) * (len(COARSE) + 1)
HOLDOUT_INDICES = (1, 3, 5, 7)
MAX_ERROR_EV = .25
MAX_DRIFT_EV = .125
MAX_STAGE_Y_SPAN = CONTRACT_MATRIX["exposure-sweep-analysis/v1"]["max_stage_center_y_span"]
CONTRACT_MATRIX["exposure-sweep-analysis/v1"]["reference_intervals"] = {
    "anchor": "before_lower_bound_plus_preserved_width_no_midpoint",
    "drift": "reject_disjoint_intervals_over_0.125EV_and_require_image_bracket_proof",
    "acceptance": "entire_prediction_minus_reference_interval_within_0.25EV",
}
CONTRACT_MATRIX["exposure-sweep-check/v1"] = {
    **CONTRACT_MATRIX[SOURCE], "states": [13], "coarse": [4], "stages": 5,
    "purpose": "diagnose_first_unity_ISP_boundary_not_calibration",
}
CONTRACT_MATRIX["exposure-sweep-check-session/v1"] = {
    **CONTRACT_MATRIX["exposure-sweep-session/v1"], "readiness_deadline_seconds": 180,
}
CONTRACT_MATRIX["exposure-sweep-check/v2"] = {
    **CONTRACT_MATRIX["exposure-sweep-check/v1"],
    "transition_probe": {"phase": 5, "frames": list(range(8)),
        "registers": ["32d4", "32d6", "32d8", "32da"],
        "boundaries": ["before_arm", "after_arm", "after_start", "after_wait", "after_stop"],
        "max_extra_reads": 160, "writes": 0,
        "read_failure": "stop_and_restore", "drift_check": "unchanged_before_measured_frame"},
}
CONTRACT_MATRIX["exposure-sweep-check/v3"] = {
    **CONTRACT_MATRIX["exposure-sweep-check/v2"],
    "live_sequencer": {"mcu_address": "a102", "clear_mask": 5,
        "allowed_values": "saved_byte_or_saved_byte_and_not_5", "max_writes": 2,
        "sequence": "save_after_reference_then_disable_then_verify_each_measured_frame_then_restore",
        "failure": "stop_and_attempt_original_controls_ISP_and_sequencer_restore"},
    "local_internal_fields": ["sweep_seq_gate", "sweep_seq_saved", "sweep_seq_target"],
}
CONTRACT_MATRIX["exposure-sweep-check/v4"] = {
    **CONTRACT_MATRIX["exposure-sweep-check/v3"],
    "awb_force_unity": {"mcu_address": "a355", "set_mask": 32, "max_writes": 2,
        "allowed_values": "saved_byte_or_saved_byte_or_32",
        "sequence": "save_then_set_before_unity_then_restore_before_native_reference_or_on_failure",
        "proof": "mode_and_four_ISP_values_before_after_each_measured_frame"},
    "local_internal_fields": CONTRACT_MATRIX["exposure-sweep-check/v3"]["local_internal_fields"] +
        ["sweep_awb_force", "sweep_awb_saved", "sweep_awb_target"],
    "transition_probe": {**CONTRACT_MATRIX["exposure-sweep-check/v2"]["transition_probe"],
        "mcu_registers": ["a102", "a104", "a355", "a34e", "a34f", "a350", "221f"],
        "max_extra_reads": 440, "atomic_snapshot": False},
    "measured_mcu_proof": "same_seven_MCU_registers_before_and_after_each_frame_no_duplicate_221f",
}
CONTRACT_MATRIX["exposure-isp-diagnostic-report/v1"] = {
    "compute": False, "lane_admission": False, "override": None, "proof": "local_trace",
    "input_source": "exposure-sweep-check/v4", "calibration_validated": False,
    "measured_frames": [8,9,10,11], "sequencer_states": [3,7],
    "same_sequencer_state_required": True, "physical_IFP_required": 128,
    "original_integrity": "CRC32_SHA256", "unknown_or_missing_proof": "reject",
}

# New sources keep the previous evidence contracts immutable.
CHECK_SOURCE = "exposure-sweep-check/v5"
REFRESH_SOURCE = "exposure-sweep-awb-refresh/v1"
CONTRACT_MATRIX[CHECK_SOURCE] = {
    **CONTRACT_MATRIX["exposure-sweep-check/v4"],
    "transition_probe": {
        **CONTRACT_MATRIX["exposure-sweep-check/v4"]["transition_probe"],
        "mcu_registers": ["a102", "a103", "a104", "a355", "a34e", "a34f", "a350", "221f"],
        "max_extra_reads": 480,
    },
    "measured_mcu_proof": "eight_MCU_registers_before_after_no_duplicate_221f_command_zero",
}
CONTRACT_MATRIX[REFRESH_SOURCE] = {
    **CONTRACT_MATRIX[CHECK_SOURCE],
    "purpose": "controlled_AWB_driver_on_force_unity_refresh_not_calibration_or_retry",
    "live_sequencer": {
        **CONTRACT_MATRIX[CHECK_SOURCE]["live_sequencer"],
        "allowed_values": "saved_byte_or_(saved_byte_and_not_5)_or_4",
        "clear_mask": 5, "set_mask": 4,
        "sequence": "save_after_reference_enable_AWB_only_then_force_unity_refresh_and_verify_then_restore",
    },
    "preview_awb": {"mcu_address": "a11f", "target": 1, "max_writes": 2,
        "sequence": "save_zero_before_enable_restore_saved_on_all_exits",
        "reason": "refresh_must_not_reload_AWB_off_preview_configuration"},
    "awb_force_unity": {**CONTRACT_MATRIX[CHECK_SOURCE]["awb_force_unity"],
        "verify_mask": 252, "restore_verify_mask": 252, "status_bits": [0, 1]},
    "refresh": {"mcu_address": "a103", "command": 5, "max_writes": 2,
        "precondition": "command_zero_and_state_3_or_7",
        "poll_limit": 100, "poll_sleep_ns": 10000000, "stable_samples": 2,
        "completion": "command_zero_same_original_state_twice_and_AE_off_AWB_on_force_unity",
        "restore": "preview_AWB_saved_then_AWB_MODE_saved_then_SEQ_MODE_saved_then_refresh_once",
        "failure": "stop_and_restore_no_measurement_retry_no_command_retry"},
    "local_internal_fields": CONTRACT_MATRIX[CHECK_SOURCE]["local_internal_fields"] +
        ["sweep_refresh_active", "sweep_preview_awb_saved", "sweep_refresh_writes"],
}
CONTRACT_MATRIX["exposure-sweep-awb-refresh-session/v1"] = {
    **CONTRACT_MATRIX["exposure-sweep-check-session/v1"],
    "input_source": REFRESH_SOURCE,
}
REFRESH_SOURCE = "exposure-sweep-awb-refresh/v2"
CONTRACT_MATRIX[REFRESH_SOURCE] = {
    **CONTRACT_MATRIX["exposure-sweep-awb-refresh/v1"],
    "awb_force_unity": {**CONTRACT_MATRIX["exposure-sweep-awb-refresh/v1"]["awb_force_unity"],
        "verify_mask": 96, "restore_verify_mask": 96,
        "defined_control_bits": [5, 6], "uninterpreted_low_bits": [2, 3, 4],
        "evidence": "batch10_command_completed_but_A355_34_to_42_and_restore_2_to_26"},
}
CONTRACT_MATRIX["exposure-sweep-awb-refresh-session/v2"] = {
    **CONTRACT_MATRIX["exposure-sweep-awb-refresh-session/v1"], "input_source": REFRESH_SOURCE,
}
CONTRACT_MATRIX["exposure-isp-diagnostic-report/v2"] = {
    **CONTRACT_MATRIX["exposure-isp-diagnostic-report/v1"],
    "input_sources": ["exposure-sweep-check/v4", CHECK_SOURCE, "exposure-sweep-awb-refresh/v1", REFRESH_SOURCE],
    "identity": {"sensor": "1", "phase": "5", "label": "sweep_s13_c0004"},
    "live_driver_mask": 5, "live_driver_expected": {CHECK_SOURCE: 0, REFRESH_SOURCE: 4},
    "command_zero_required": True,
    "refresh_proof": "two_successful_command5_writes_apply_then_restore_each_bounded_2_to_100_polls",
    "gain_model": "report_round_AWB_times_observed_221f_over_128_without_assuming_221f_is_162",
}

MANUAL_SOURCE = "exposure-sweep-post-refresh-manual/v1"
CONTRACT_MATRIX[MANUAL_SOURCE] = {
    **CONTRACT_MATRIX[REFRESH_SOURCE],
    "purpose": "normalize_AWB_then_disable_drivers_before_final_physical_writes_single_S13",
    "live_sequencer": {**CONTRACT_MATRIX[REFRESH_SOURCE]["live_sequencer"],
        "allowed_values": "saved_byte_or_AWB_on_AE_off_or_both_off",
        "max_writes": 3,
        "sequence": "enable_AWB_refresh_verify_unit_AWB_disable_preview_and_live_AWB_then_final_physical_writes"},
    "preview_awb": {**CONTRACT_MATRIX[REFRESH_SOURCE]["preview_awb"],
        "max_writes": 3, "sequence": "save_zero_enable_for_refresh_disable_before_manual_restore_on_exit"},
    "post_refresh_manual": {
        "precondition": "refresh_completed_AWB_RGB_128_command_zero_state_3_or_7",
        "measurement_live_driver_bits": 0, "preview_awb": 0,
        "disable_verification": "immediate_readback_no_fallback_refresh",
        "final_write_order": ["3012=4", "S13_core_codes", "32d4=128", "32d6=128", "32d8=128", "32da=128"],
        "refresh_after_final_writes": "forbidden_until_measurement_ends_and_restore_begins",
        "coarse_required": 4, "first_frame": "retained_as_settle_diagnostic_not_measurement",
        "failure": "stop_and_restore_no_rewrite_no_retry",
    },
    "transition_probe": {**CONTRACT_MATRIX[REFRESH_SOURCE]["transition_probe"],
        "registers": ["32d4", "32d6", "32d8", "32da", "3012"], "max_extra_reads": 520},
    "local_internal_fields": CONTRACT_MATRIX[REFRESH_SOURCE]["local_internal_fields"] + ["sweep_manual_locked"],
}
CONTRACT_MATRIX["exposure-sweep-post-refresh-manual-session/v1"] = {
    **CONTRACT_MATRIX["exposure-sweep-awb-refresh-session/v2"], "input_source": MANUAL_SOURCE,
}
CONTRACT_MATRIX["exposure-isp-diagnostic-report/v3"] = {
    **CONTRACT_MATRIX["exposure-isp-diagnostic-report/v2"],
    "input_sources": CONTRACT_MATRIX["exposure-isp-diagnostic-report/v2"]["input_sources"] + [MANUAL_SOURCE],
    "manual_proof": "unit_AWB_before_disable_live_bits_zero_final_writes_after_refresh_no_refresh_during_measurement_coarse4",
    "missing_states": "reject_missing_proof_without_inventing_a_sequencer_transition",
}

UPDATE_SOURCE = "exposure-isp-update/v1"
UPDATE_ARMS = [("restart",96),("restart",128),("restart",160),("continuous",128)]
CONTRACT_MATRIX[UPDATE_SOURCE] = {
    **CONTRACT_MATRIX[MANUAL_SOURCE],
    "purpose": "observe_initial_load_versus_repeated_update_not_calibration",
    "stages": 6, "arms": UPDATE_ARMS, "frames_per_arm": 3,
    "setup": "seed_native_reference_then_fresh_normalize_refresh_and_disable_AWB_for_each_arm",
    "live_sequencer": {**CONTRACT_MATRIX[MANUAL_SOURCE]["live_sequencer"], "max_writes": 9},
    "preview_awb": {**CONTRACT_MATRIX[MANUAL_SOURCE]["preview_awb"], "max_writes": 9},
    "refresh": {**CONTRACT_MATRIX[MANUAL_SOURCE]["refresh"], "max_writes": 5},
    "diagnostic_writes": {
        "IFP_initial": [96,128,160], "IFP_after_first_frame": 128,
        "per_arm_IFP_channel_writes": 8, "coarse": 4, "core_code": 528,
        "rewrite": "one_planned_perturbation_after_frame0_not_retry_or_calibration_repair",
        "frame2": "no_further_rewrite",
    },
    "observation": {
        "IFP": "successful_immediate_write_readback_then_observe_1_to_512_no_unity_pass_claim",
        "controls": "AE_AWB_off_force_unity_AWB_RGB128_command_zero_state3_coarse4_core528_fail_closed",
        "registers": ["32d4","32d6","32d8","32da","3012","3056","3058","305a","305c","a102","a103","a104","a355","a34e","a34f","a350","221f"],
        "continuous": "one_start_one_outstanding_DMA_rearm_next_before_reads_and_rewrite_no_queue",
        "restart": "stop_each_frame_rewrite_while_stopped_restart_next_frame",
        "frame1": "may_overlap_continuous_rewrite_use_frame2_for_fresh_post_rewrite_frame",
        "max_state_groups_per_arm": 18, "max_state_reads": 1088, "max_mode_reads": 228,
        "mode_registers": ["2703","2705","2707","2709","2715","2717","2719","271b","271d","271f","2721","272b","272d","2731","2733","2735","2737","2755","2757"],
        "mode_snapshots": ["before_initial","after_frame0","after_frame2"],
        "mode_writes": 0, "arbitrary_RAM_or_XDMA_writes": 0,
        "failure": "stop_close_outstanding_event_restore_no_fallback_or_capture_retry",
    },
    "raw": "last_frame_center_128x96_YUYV_per_arm_after_stop",
    "local_internal_fields": CONTRACT_MATRIX[MANUAL_SOURCE]["local_internal_fields"],
}
CONTRACT_MATRIX["exposure-isp-update-session/v1"] = {
    **CONTRACT_MATRIX["exposure-sweep-post-refresh-manual-session/v1"], "input_source": UPDATE_SOURCE,
}
CONTRACT_MATRIX["exposure-isp-update-report/v1"] = {
    "compute": False, "lane_admission": False, "override": None, "proof": "local_trace",
    "input_source": UPDATE_SOURCE, "calibration_validated": False,
    "verification": "CRC_SHA_identity_fixed_arms_and_frames_verified_writes_DMA_and_control_reads",
    "IFP_drift": "observation_only_never_relabel_as_unity_or_calibration_success",
    "continuous_start_count": 1, "restart_start_count": 3,
    "call_count_scope": "first_arm_SetReceiving_through_final_arm_StopCapture_excludes_later_camera_cleanup",
    "causation": "repeated_observation_is_not_identification_of_writer_or_shadow_address",
}

NO_RECEIVE_SOURCE = "no-receive-frame-update/v1"
CONTRACT_MATRIX[NO_RECEIVE_SOURCE] = {
    **CONTRACT_MATRIX[UPDATE_SOURCE],
    "purpose": "separate_new_SetReceiving_from_repeated_IFP_update_no_calibration",
    "stages": 4, "arms": ["continuous_DMA_control", "no_new_receive"],
    "live_sequencer": {**CONTRACT_MATRIX[UPDATE_SOURCE]["live_sequencer"], "max_writes": 5},
    "preview_awb": {**CONTRACT_MATRIX[UPDATE_SOURCE]["preview_awb"], "max_writes": 5},
    "refresh": {**CONTRACT_MATRIX[UPDATE_SOURCE]["refresh"], "max_writes": 3},
    "no_receive": {
        "initial_DMA_frames": 1, "SetReceiving_count": 1, "StartCapture_count": 1,
        "pending_receive_proof": "WaitFrame_zero_IsFinishedReceiving_true_close_DMA_handle_before_rewrite",
        "rewrite": "exactly_once_four_IFP_128_verified_after_first_DMA",
        "VSYNC_observations": 3, "wait_timeout_ns": 2000000000,
        "VSYNC_proof": "clear_event_then_wait_zero_and_strictly_new_positive_GetLatestVsyncTiming",
        "timestamp_units": "raw_service_value_no_assumed_unit",
        "no_new_calls": ["SetReceiving","StartCapture","StopCapture","SwitchContext","ClearBuffer","Refresh"],
        "window": "DMA_completion_through_last_verified_VSYNC_then_stop_restore",
        "red_reads_per_VSYNC": 8, "red_read_address": "32d4",
        "state_registers": CONTRACT_MATRIX[UPDATE_SOURCE]["observation"]["registers"],
        "max_observation_register_reads": 313,
        "failure": "inconclusive_stop_restore_no_time_only_frame_claim_no_retry",
    },
    "control": {"implementation": "batch13_continuous_DMA_without_Mode_reads", "DMA_frames": 3},
    "mode_reads": 0,
    "raw": "control_last_DMA_and_no_receive_first_DMA_ROI_never_VSYNC_image_claim",
    "local_internal_fields": CONTRACT_MATRIX[UPDATE_SOURCE]["local_internal_fields"],
}
CONTRACT_MATRIX["no-receive-frame-update-session/v1"] = {
    **CONTRACT_MATRIX["exposure-isp-update-session/v1"], "input_source": NO_RECEIVE_SOURCE,
}
CONTRACT_MATRIX["no-receive-frame-update-report/v1"] = {
    "compute": False, "lane_admission": False, "override": None, "proof": "local_trace",
    "input_source": NO_RECEIVE_SOURCE, "calibration_validated": False,
    "verification": "CRC_SHA_DMA_complete_closed_handle_new_VSYNC_timestamps_no_new_receive_fixed_controls_restoration",
    "failed_or_stale_VSYNC": "inconclusive_never_equate_waiting_time_to_frames",
}

CONTRACT_MATRIX["camera-writer-identity/v1"] = {
    "compute": True, "lane_admission": False,
    "override": "standalone_hardware_diagnostic", "proof": "local_trace",
    "purpose": "read_current_Camera_process_text_before_using_archived_trace_addresses",
    "process_identity": "fresh_GDB_OS_process_list_exact_name_camera_single_match",
    "attachments": 1, "attachment_retries": 0,
    "read_range": [0x100000, 0x115000], "read_chunk_bytes": 512,
    "identity": "full_text_SHA256_and_all_registered_16_byte_entry_signatures",
    "writes": 0, "breakpoints": 0, "continue": "detach_only_after_read_no_capture",
    "pause_deadline_seconds": 30, "command_timeout_seconds": 4,
    "failure": "attempt_detach_in_finally_report_unknown_identity_no_trace_or_retry",
    "local_internal_fields": ["attachment_attempted", "attached", "detached", "identity_matched", "captured_bytes", "pause_seconds"],
}

WRITER_WINDOW_SOURCE = "camera-writer-window/v1"
CONTRACT_MATRIX[WRITER_WINDOW_SOURCE] = {
    **CONTRACT_MATRIX[NO_RECEIVE_SOURCE], "stages": 3, "arms": ["no_new_receive"],
    "purpose": "one_no_receive_window_synchronized_with_external_Camera_syscall_observer",
    "refresh": {**CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['refresh'], "max_writes": 2},
    "live_sequencer": {**CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['live_sequencer'], "max_writes": 3},
    "preview_awb": {**CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['preview_awb'], "max_writes": 3},
    "gate": {"port": 5001, "connections": 1, "backlog": 1,
        "ready": "FILMTRACE1 READY\n", "start": "GO\n",
        "done": "FILMTRACE1 DONE", "release": "RELEASE\n",
        "accept_timeout_ms": 120000, "start_timeout_ms": 120000,
        "release_timeout_ms": 15000, "writes_to_device_memory": 0,
        "send_timeout_ms": 5000,
        "sequence": "baseline_normalize_disable_final_controls_then_READY_GO_one_DMA_rewrite_VSYNC_stop_DONE_RELEASE_restore",
        "failure": "no_GO_no_capture_or_retry_attempt_original_restore_close_socket_SOC",
        "hardware_no_calls_between": "READY_to_GO_and_DONE_to_RELEASE",
    },
    "local_internal_fields": CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['local_internal_fields'],
}
CONTRACT_MATRIX["camera-writer-trace/v1"] = {
    "compute": True, "lane_admission": False,
    "override": "standalone_hardware_diagnostic", "proof": "local_trace",
    "input_source": WRITER_WINDOW_SOURCE, "deployments": 1, "retries": 0,
    "identity": "fresh_process_name_camera_full_text_SHA256_entry_signature_before_attach_trace",
    "observer": "Camera_only_QCatchSyscalls_SendSyncRequest_0x32_entry_and_return",
    "breakpoints": 0, "memory_writes": 0, "target_function_calls": 0,
    "arm": "verify_I2C_handle_enable_syscall_catch_resume_Camera_then_GO",
    "end": "DONE_then_interrupt_disable_catch_detach_then_RELEASE",
    "deadline_seconds": 120, "max_stops": 3000,
    "deadline_scope": "GO_to_DONE", "identity_deadline_seconds": 30,
    "IPC_words": 64, "buffer_max_bytes": 2048,
    "IFP_stack_bytes": 128,
    "I2C_handle": "read_pointer_at_0011d638_then_dereference_checked_0011d000_0011e000",
    "identity_read_bytes": 86016,
    "positive_control": "all_four_IFP128_write_entries_plus_device_successful_write_readback",
    "negative_evidence": "no_claim_if_trace_gap_cap_timeout_unknown_write_command_missing_buffer_or_no_positive_control",
    "scope_limit": "Camera_outbound_IPC_only_not_all_processes_or_physical_bus_monitor",
    "local_internal_fields": ["attached", "catch_enabled", "running", "stops", "tls_cache", "i2c_handle", "inflight_by_thread"],
    "inflight_limit": 8, "buffer_read": "bounded_readonly_IPC_static_buffers_before_svc",
    "rendezvous_readiness": {"deadline_seconds": 180, "poll_seconds": 1, "accepted_connections": 1},
}
CONTRACT_MATRIX["camera-writer-trace-session/v1"] = {
    **CONTRACT_MATRIX['no-receive-frame-update-session/v1'],
    "input_source": WRITER_WINDOW_SOURCE, "readiness_deadline_seconds": 360,
}
CONTRACT_MATRIX['camera-writer-report/v1'] = {
    "compute": False, "lane_admission": False, "override": None, "proof": "local_trace",
    "input_source": WRITER_WINDOW_SOURCE, "observer_source": 'camera-writer-trace/v1',
    "calibration_validated": False,
    "verification": "CRC_SHA_device_window_new_VSYNC_fixed_controls_write_readback_restoration_trace_pairing_identity_gate_positive_control",
    "covered_I2C_commands": [4,8,10,12],
    "unknown_I2C_command_or_missing_trace": "inconclusive_never_negative_writer_claim",
    "no_external_write_claim": "Camera_outbound_only_never_all_processes_or_bus",
}

# v1 remains an incomplete instrumented experiment. v2 observes readback drift
# under debugger stops, while all write/IPC and fixed-control checks stay strict.
WRITER_OBSERVE_SOURCE = 'camera-writer-window/v2'
CONTRACT_MATRIX[WRITER_OBSERVE_SOURCE] = {
    **CONTRACT_MATRIX[WRITER_WINDOW_SOURCE],
    'instrumented_rewrite': {
        'writes': 'exactly_four_IFP128_once_after_first_completed_DMA',
        'write_result': 'strict_success_both_device_call_and_observed_I2C_service_reply',
        'readback': 'raw_observation_range1_512_may_already_be_overwritten_under_debugger',
        'positive_control': 'pre_GO_all_four128_verified_plus_under_trace_all_four128_write_requests_and_successful_service_replies',
        'calibration_validated': False,
        'older_unity_sources': 'unchanged_strict128_readback_failure',
    },
}
CONTRACT_MATRIX['camera-writer-trace/v2'] = {
    **CONTRACT_MATRIX['camera-writer-trace/v1'], 'input_source': WRITER_OBSERVE_SOURCE,
    'positive_control': CONTRACT_MATRIX[WRITER_OBSERVE_SOURCE]['instrumented_rewrite']['positive_control'],
    'syscall_return': 'retain_register_r0_raw_do_not_label_transport_Result_use_IPC_reply_word1_and_device_calls',
    'RSP_command_framing': 'drain_up_to8_queued_async_stop_or_output_packets_before_reply_never_resend_command',
    'cleanup': 'attempt_detach_even_if_catch_disable_fails_record_each_result',
    'local_internal_fields': CONTRACT_MATRIX['camera-writer-trace/v1']['local_internal_fields']+['async_packets_drained'],
}
CONTRACT_MATRIX['camera-writer-trace-session/v2'] = {
    **CONTRACT_MATRIX['camera-writer-trace-session/v1'], 'input_source': WRITER_OBSERVE_SOURCE,
}
CONTRACT_MATRIX['camera-writer-report/v2'] = {
    **CONTRACT_MATRIX['camera-writer-report/v1'], 'input_source': WRITER_OBSERVE_SOURCE,
    'observer_source': 'camera-writer-trace/v2',
    'instrumented_rewrite': CONTRACT_MATRIX[WRITER_OBSERVE_SOURCE]['instrumented_rewrite'],
    'syscall_register_r0': 'raw_only_no_transport_result_claim',
}


# HG isolation and timing observations are a distinct compute entry. No GDB,
# firmware writes, arbitrary RAM search, or modification of Gamma/CCM tables.
HG_SOURCE = 'camera-hg-isolation/v1'
HG_DELAYS = (25, 75, 125, 175)
HG_MCU = ['a120','ab04','ab06','ab08','ab17','2b1b','ab1f','ab37','2b38','2b3a',
          'a353','a354','2356','2358']
HG_MCU += [f'{a:04x}' for a in range(0x2332,0x2348,2)]
HG_MCU += [f'{a:04x}' for a in range(0xab3c,0xab62)]
HG_PHYSICAL = ['3210','32c0','32c2','337c','337e']
HG_PHYSICAL += [f'{a:04x}' for a in range(0x3604,0x3640,2)]
CONTRACT_MATRIX[HG_SOURCE] = {
    **CONTRACT_MATRIX[NO_RECEIVE_SOURCE], 'stages': 5,
    'purpose': 'HG_on_off_on_and_VSYNC_phase_perturbations_not_calibration',
    'arms': [1,0,1], 'setup': 'fresh_AWB_normalize_each_arm_then_disable_AE_AWB_then_HG_gate_then_final_physical_writes',
    'fixed_AE_dgain':128,
    'physical_setup_writes':'existing_prepare_then_final_five_controls_and_four_IFP_after_HG_gate',
    'IFP_writes_per_arm':24, # prepare4 + final4 + four_trials*4
    'HG_gate': {'address':'a102','mask':8,'values':'(saved_and_not_5)_with_bit3_as_arm',
                'writes_per_arm':1,'verify':'immediate_and_every_full_state','refresh_after_gate':False,
                'preview_HG':'a120_readonly_no_inferred_driver_effect_from_gate_readback'},
    'refresh': {**CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['refresh'], 'max_writes':4},
    'live_sequencer': {**CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['live_sequencer'], 'max_writes':10},
    'preview_awb': {**CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['preview_awb'], 'max_writes':7},
    'snapshot': {'mcu':HG_MCU,'physical':HG_PHYSICAL,'boundaries':['before_stream','after_stop'],
                 'writes':0,'atomic':False,'eight_bit_range_checked':True,
                 'source':'BlocksDS_libnds_camera_h_and_MT9V113_D_driver_ID11'},
    'timing': {'delays_ms':HG_DELAYS,'poll_limit':64,'poll_sleep_ns':4000000,
               'initial_DMA_frames':1,'SetReceiving_count':1,'StartCapture_count':1,
               'rewrite_per_trial':'four128_once_strict_write_result_observational_readback_1_to512',
               'poll':'32d4_until_first_non128_or_bound_no_retry',
               'VSYNC':'clear_wait_and_strictly_advancing_positive_raw_service_timestamp_each_trial',
               'clock':'osGetTime_ms_read_start_end_bounds_not_actual_write_time',
               'sleep_result':'libctru_void_zero_call_record_means_issued_only_actual_elapsed_is_read_from_clock',
               'phase':'delay_relative_to_observed_VSYNC_return_not_assumed_exact_sensor_phase',
               'no_calls':'new_receive_restart_refresh_context_changes_inside_window',
               'evidence_limit':'finite_observation_HG_bit_readback_does_not_prove_internal_driver_stopped'},
    'raw':'one_initial_DMA_ROI_per_arm_never_post_rewrite_image_claim',
    'local_internal_fields':CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['local_internal_fields'],
}
CONTRACT_MATRIX['camera-hg-isolation-session/v1'] = {
    **CONTRACT_MATRIX['no-receive-frame-update-session/v1'], 'input_source':HG_SOURCE,
    'readiness_deadline_seconds':360,
}
CONTRACT_MATRIX['camera-hg-isolation-report/v1'] = {
    'compute':False,'lane_admission':False,'override':None,'proof':'local_trace',
    'input_source':HG_SOURCE,'calibration_validated':False,
    'verification':'CRC_SHA_registered_arms_gate_readbacks_fixed_controls_DMA_closed_VSYNC_poll_bounds_snapshot_restore',
    'unknown_missing_or_extra_work':'reject','clock_model':'report_brackets_and_raw_VSYNC_no_timer_or_frame_cause_claim',
    'HG_model':'on_off_on_association_not_proof_of_specific_firmware_function',
}


# Independent single-variable experiment. Bit6 interpretation is a hypothesis
# from the MT9D111 manufacturer variable reference, not a proven MT9V113 formula.
CCM_SOURCE = 'camera-ccm-normalization/v1'
CONTRACT_MATRIX[CCM_SOURCE] = {
    **CONTRACT_MATRIX[HG_SOURCE],
    'purpose':'test_A355_bit6_normalization_hypothesis_with_HG_off_not_calibration',
    'arms':[0,0,0], 'normalization_disable':[0,1,0],
    'labels':['ccm_0_normalized','ccm_1_disabled','ccm_2_normalized'],
    'awb_control':{'address':'a355','mask':64,'force_unity':32,
        'target':'(saved_or32)_and_not64_then_or_arm_bit6',
        'max_writes':5,'sequence':'baseline_force_then_each_arm_target_before_AWB_on_Refresh_then_saved_restore',
        'proof':'immediate_mask96_readback_before_each_Refresh_and_fixed_mask96_each_state',
        'hypothesis_source':'MT9D111_SOC2010DS_table13_AWB_mode_bit6_homolog_not_MT9V113_firmware_proof'},
    'setup':'each_arm_A355_target_before_AWB_on_Refresh_unit_AWB_disable_AE_AWB_then_HG_off_then_final_physical_writes',
    'timing':{**CONTRACT_MATRIX[HG_SOURCE]['timing'],'rewrite_targets':[96,128,160,128],
        'poll':'32d4_until_first_value_different_from_that_trial_target_or_bound_no_retry',
        'purpose':'separate_changed_reload_value_from_no_reload_when_trial_target_is_unity'},
    'HG_gate':{**CONTRACT_MATRIX[HG_SOURCE]['HG_gate'],'values':'saved_and_not13_all_three_arms'},
    'awb_force_unity':{**CONTRACT_MATRIX[HG_SOURCE]['awb_force_unity'],
        'max_writes':5,'allowed_values':'saved_or_registered_per_arm_force32_bit6_target'},
    'snapshot':{**CONTRACT_MATRIX[HG_SOURCE]['snapshot'], 'mcu':HG_MCU+[f'{a:04x}' for a in range(0xa365,0xa36c)]},
    'shared_component':'HG_bounded_DMA_closed_VSYNC_phase_poll_and_readonly_snapshots_same_v1_limits',
}
CONTRACT_MATRIX['camera-ccm-normalization-session/v1'] = {
    **CONTRACT_MATRIX['camera-hg-isolation-session/v1'],'input_source':CCM_SOURCE,
}
CONTRACT_MATRIX['camera-ccm-normalization-report/v1'] = {
    **CONTRACT_MATRIX['camera-hg-isolation-report/v1'],'input_source':CCM_SOURCE,
    'verification':'HG_shared_evidence_plus_three_exact_A355_control_writes_before_arm_Refresh_and_per_arm_mask96',
    'conclusion':'bit6_dependency_if_reversible_not_full_CCM_formula_or_firmware_writer_identification',
}


# v1 remains incomplete at its first signed-CCM sentinel error. v2 never
# reinterprets that error. It uses successful logical-byte reads for all CCM
# words, with a native-word reference proving the alias mapping at each snapshot.
CCM_BYTES_SOURCE = 'camera-ccm-normalization/v2'
CCM_WORDS = [f'{a:04x}' for a in range(0x2332,0x2348,2)]
CONTRACT_MATRIX[CCM_BYTES_SOURCE] = {
    **CONTRACT_MATRIX[CCM_SOURCE],
    'snapshot':{**CONTRACT_MATRIX[CCM_SOURCE]['snapshot'],
        'mcu':[a for a in CONTRACT_MATRIX[CCM_SOURCE]['snapshot']['mcu'] if a not in CCM_WORDS],
        'derived_words':CCM_WORDS,'native_reference':'2332',
        'word_transport':'two_successful_logical8_reads_(word_or8000)_and_(word_or8000_plus1)_big_endian',
        'alias_proof':'native2332_read_success_equal_reconstructed2332_each_of_six_snapshots',
        'failure':'stop_restore_on_any_byte_error_width_over255_reference_error_or_mismatch_no_fallback',
        'sentinel':'FFFF_is_derived_only_from_successful_FF_FF_never_from_error_result',
        'physical_bus':'still16bit_I2C_word_transactions_logical_variable_access_width_only',
        'atomic':False,'physical_read_calls_per_snapshot':117,
        'source':'MT9V113_MCU_variable_width_encoding_plus_live_native2332_crosscheck'},
    'runtime_internal_fields':'none_added',
}
CONTRACT_MATRIX['camera-ccm-normalization-session/v2'] = {
    **CONTRACT_MATRIX['camera-ccm-normalization-session/v1'],'input_source':CCM_BYTES_SOURCE,
}
CONTRACT_MATRIX['camera-ccm-normalization-report/v2'] = {
    **CONTRACT_MATRIX['camera-ccm-normalization-report/v1'],'input_source':CCM_BYTES_SOURCE,
    'verification':'v1_control_timing_and_integrity_plus132_successful_byte_reads_66_derived_words_six_native_reference_checks',
    'historical_v1':'unchanged_exclusive_word_read_error_still_incomplete_not_accepted_as_FFFF',
}


# Persistent startup diagnostics are a new source, not a RAM-only v2 bypass.
CCM_BOOT_SOURCE = 'camera-ccm-normalization/v3'
BOOT_STAGES = {1:'MAIN_ENTER',2:'GFX_READY',3:'REQUEST_OK',4:'RAM_ALLOC_BEGIN',5:'RAM_ALLOC_OK',
    6:'LOGS_READY',7:'PIXELS_BEGIN',8:'PIXELS_READY',9:'CAMERA_BEGIN',10:'CAMERA_RESULT',
    11:'PREFLIGHT_BEGIN',12:'PREFLIGHT_RESULT',13:'ARM_BEGIN',14:'ARM_READY',
    15:'SNAPSHOT_BEGIN',16:'BYTE_BEGIN',17:'BYTE_RESULT',18:'SNAPSHOT_END',
    19:'CAMERA_CLOSE',20:'CAMERA_CLOSED',21:'RUN_END',22:'BEFORE_FREEZE',23:'HTTP_BEGIN',24:'EXIT',25:'APT_EVENT',255:'FAULT'}
CONTRACT_MATRIX[CCM_BOOT_SOURCE] = {
    **CONTRACT_MATRIX[CCM_BYTES_SOURCE],
    'startup_journal':{'path':'sdmc:/3ds/Film3DS/ccm-boot-v3.log','mode':'append_only_never_truncate',
        'total_cap_bytes':2097152,'max_records_per_run':768,'max_record_bytes':160,
        'stages':BOOT_STAGES,'writes':'synchronous_fixed_records_no_retry_or_queue',
        'memory':'same_28MiB_arena_614400_linear_pixels_later1MiB_SOC_no_new_bulk_buffer',
        'APT_events':'hook_type_recorded_report_inconclusive_if_any_event_before_freeze',
        'fault':'best_effort_raw_SD_record_before_existing_fatal_hold_no_execution_recovery',
        'RAM_copy':'current_run_prefix_before_freeze_to_startup_journal.txt_cap128KiB',
        'post_freeze':'HTTP_BEGIN_EXIT_best_effort_SD_only_never_change_frozen_RAM_or_repeat_capture',
        'failure':'stop_and_restore_when_camera_open_show_held_error_no_RAM_only_fallback',
        'missing_MAIN':'startup_before_main_or_journal_open_failure_remains_unknown',
        'source_selection':'exact_v3_scope_argument_before_graphics_no_old_scope_journal'},
    'storage':'RAM_images_plus_bounded_local_SD_startup_journal',
    'local_internal_fields':CONTRACT_MATRIX[CCM_BYTES_SOURCE]['local_internal_fields']+
        ['boot_journal','boot_stage','boot_address','boot_result','boot_heap_bytes','boot_linear_bytes','boot_run_ms'],
}
CONTRACT_MATRIX['camera-ccm-normalization-session/v3'] = {
    **CONTRACT_MATRIX['camera-ccm-normalization-session/v2'],'input_source':CCM_BOOT_SOURCE,
}
CONTRACT_MATRIX['camera-ccm-normalization-report/v3'] = {
    **CONTRACT_MATRIX['camera-ccm-normalization-report/v2'],'input_source':CCM_BOOT_SOURCE,
    'extra_proof':'bounded_current_run_journal_sequence_MAIN_through_RUN_END_and_BEFORE_FREEZE_no_FAULT',
    'missing_journal_or_device_files':'inconclusive_no_claim_of_completed_acquisition_or_restore',
}


GAIN_MATRIX_SOURCE = 'camera-gain-matrix/v1'
GAIN_MATRIX_NODES = [dict(index=i,fps=fps,gate=gate,coarse=coarse) for i,(fps,gate,coarse) in enumerate(
    (fps,gate,coarse) for fps in (5,15) for gate in (2,0) for coarse in (4,528))]
CONTRACT_MATRIX[GAIN_MATRIX_SOURCE] = {
    **CONTRACT_MATRIX[CCM_BOOT_SOURCE],
    'purpose':'one_coherent_bounded_gain_exposure_driver_fps_matrix_with_adaptive_observation_not_calibration',
    'stages':10,'nodes':GAIN_MATRIX_NODES,'trial_targets':[128,96,160],
    'adaptive':{'first_target_cycles':8,'later_cycles_if_first_held_in_VSYNC_and_DMA':8,
        'later_cycles_if_first_overwritten':2,'decision_proof':'all_first_target_states_and_trailing_DMA_actual_IFP',
        'fixed_control_or_transport_error':'stop_whole_run_restore_no_retry_or_skipped_failed_node'},
    'VSYNC':{'period_samples':3,'consecutive_step_tolerance_percent':20,
        'proof':'cleared_event_new_positive_raw_timestamp_and_step_near_measured_profile_period',
        'units':'raw_service_steps_plus_host_ms_no_assumed_conversion','wait_ns':2000000000},
    'frames':{'initial_DMA_per_node':1,'trailing_DMA_per_trial':1,'StartCapture_per_node':1,
        'StopCapture_per_node':1,'no_receive_between_rewrite_and_last_VSYNC':True,
        'raw':'one_center_ROI_after_each_trial_trailing_DMA_with_before_after_state_proof'},
    'profiles':{'initial':5,'switch_to15_before_node':4,'restore_to5_before_original_controls':True,
        'declared_profile_calls':3,'call_scope':'cal_setup_and_matrix_profile_labels_excludes_preflight','apply_polls':100,'stable_samples':2,'poll_sleep_ns':10000000,
        'frame_lines':{5:2114,15:704},'commands_after_final_writes':'forbidden_inside_node'},
    'state_registers':CONTRACT_MATRIX[UPDATE_SOURCE]['observation']['registers']+['300a','300c'],
    'controls':{'core_code':528,'AWB_RGB':128,'A355_mask96':32,
        'A102':'saved_high_bits_preserved_clear_low4_then_or_node_gate',
        'AE_dgain':'record_each_node_baseline_1to512_then_require_unchanged'},
    'refresh':{**CONTRACT_MATRIX[CCM_BOOT_SOURCE]['refresh'],'max_writes':9},
    'live_sequencer':{**CONTRACT_MATRIX[CCM_BOOT_SOURCE]['live_sequencer'],'max_writes':25},
    'preview_awb':{**CONTRACT_MATRIX[CCM_BOOT_SOURCE]['preview_awb'],'max_writes':17},
    'awb_force_unity':{**CONTRACT_MATRIX[CCM_BOOT_SOURCE]['awb_force_unity'],'max_writes':3},
    'matrix_files':{'matrix_events.csv':512*1024,'matrix_plan.csv':64*1024},
    'snapshot':{**CONTRACT_MATRIX[CCM_BOOT_SOURCE]['snapshot'],
        'Mode_readonly':CONTRACT_MATRIX[UPDATE_SOURCE]['observation']['mode_registers']},
    'startup_journal':{**CONTRACT_MATRIX[CCM_BOOT_SOURCE]['startup_journal'],
        'max_records_per_run':1024,'source_selection':'exact_camera-gain-matrix_scope',
        'expected_prefreeze_records':768},
    'local_internal_fields':CONTRACT_MATRIX[CCM_BOOT_SOURCE]['local_internal_fields']+
        ['matrix_node','matrix_branch','matrix_period_raw','matrix_dgain_baseline','matrix_event_stream','matrix_plan_stream'],
    'max_VSYNC_observations':216,'max_node_DMA_frames':32,'max_ROI_files':24,
}
CONTRACT_MATRIX['camera-gain-matrix-session/v1'] = {
    **CONTRACT_MATRIX['camera-ccm-normalization-session/v3'],'input_source':GAIN_MATRIX_SOURCE,
    'readiness_deadline_seconds':900,
}
CONTRACT_MATRIX['camera-gain-matrix-report/v1'] = {
    'compute':False,'lane_admission':False,'override':None,'proof':'local_trace',
    'input_source':GAIN_MATRIX_SOURCE,'calibration_validated':False,
    'verification':'CRC_SHA_exact8_nodes_explicit_adaptive_cycle_choices_full_states_VSYNC_step_DMA_images_snapshots_restore_boot_journal',
    'unknown_missing_or_extra_nodes':'reject','timing_cause_identified':False,'writer_identified':False,
    'output_fields':'sequencer_target_and_trial_write_target_separate_no_shadowed_target_field',
}

# v1 is retained as the immutable contract of the incomplete Batch21 run.
GAIN_MATRIX_V1_SOURCE=GAIN_MATRIX_SOURCE
GAIN_MATRIX_SOURCE='camera-gain-matrix/v2'
CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]={
    **CONTRACT_MATRIX[GAIN_MATRIX_V1_SOURCE],
    'frames':{'initial_DMA_per_node':1,'trailing_DMA_per_trial':0,
        'StartCapture_per_node':1,'StopCapture_per_node':1,
        'no_receive_between_first_DMA_and_node_stop':True,
        'raw':'one_initial_node_ROI_before_period_and_trial_writes_not_target_specific'},
    'adaptive':{**CONTRACT_MATRIX[GAIN_MATRIX_V1_SOURCE]['adaptive'],
        'later_cycles_if_first_held_in_VSYNC_and_DMA':None,
        'later_cycles_if_first_held_in_VSYNC':8,
        'decision_proof':'immediate_readback_and_all_first_target_VSYNC_states_no_image_claim'},
    'max_node_DMA_frames':8,'max_ROI_files':8,
}
CONTRACT_MATRIX['camera-gain-matrix-session/v2']={
    **CONTRACT_MATRIX['camera-gain-matrix-session/v1'],'input_source':GAIN_MATRIX_SOURCE,
}
CONTRACT_MATRIX['camera-gain-matrix-report/v2']={
    **CONTRACT_MATRIX['camera-gain-matrix-report/v1'],'input_source':GAIN_MATRIX_SOURCE,
    'verification':'CRC_SHA_exact8_nodes_adaptive_cycles_full_states_VSYNC_steps_initial_node_images_snapshots_restore_boot',
    'image_claim':'initial_node_DMA_only_not_each_target_or_last_cycle',
}

GAIN_MATRIX_V2_SOURCE=GAIN_MATRIX_SOURCE
GAIN_MATRIX_SOURCE='camera-gain-matrix/v3'
CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]={
    **CONTRACT_MATRIX[GAIN_MATRIX_V2_SOURCE],
    'snapshot':{**CONTRACT_MATRIX[GAIN_MATRIX_V2_SOURCE]['snapshot'],
        'alias_transport_proof':{'native_word':'2703','byte_aliases':['a703','a704'],
            'expected':640,'bracket':'native_before_bytes_native_after_all_equal_or_stop'},
        'alias_proof':'static2703_native_before_bytes_native_after_all640_each_snapshot',
        'failure':'stop_restore_on_RPC_error_invalid_byte_or_static_alias_mismatch_not_live_CCM_difference',
        'physical_read_calls_per_snapshot':141,
        'source':'stable_context_width_alias_proof_plus_time_separated_live_CCM_byte_observations',
        'CCM_coherence':'record_2332_native_before_and_after_first_byte_pair_with_match_flags',
        'CCM_mismatch':'successful_time_separated_values_are_observations_not_transport_errors',
        'CCM_derived_use':'byte_observations_only_never_atomic_or_coherent_CCM_claim',
        'failed_read_or_invalid_byte':'stop_whole_matrix_no_substitution_or_retry'},
    'runtime_internal_fields':'registered_local_internal_fields_only_never_portable_metadata',
    'local_internal_fields':CONTRACT_MATRIX[GAIN_MATRIX_V2_SOURCE]['local_internal_fields']+['ccm_snapshot_coherence'],
}
CONTRACT_MATRIX['camera-gain-matrix-session/v3']={
    **CONTRACT_MATRIX['camera-gain-matrix-session/v2'],'input_source':GAIN_MATRIX_SOURCE,
}
CONTRACT_MATRIX['camera-gain-matrix-report/v3']={
    **CONTRACT_MATRIX['camera-gain-matrix-report/v2'],'input_source':GAIN_MATRIX_SOURCE,
    'CCM_proof':'stable_width_alias_transport_and_explicit_time_separated_CCM_reference_window_no_atomic_claim',
}

GAIN_MATRIX_V3_SOURCE=GAIN_MATRIX_SOURCE
GAIN_MATRIX_SOURCE='camera-gain-matrix/v4'
CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]={
    **CONTRACT_MATRIX[GAIN_MATRIX_V3_SOURCE],
    'initial_IFP':{'labels':['isp_update_initial','matrix_initial'],
        'writes':'each_four_targets128_once_RPC_success_required',
        'readback':'four_successful_1to512_observations_equality_is_subject_result_not_fixed_control',
        'fixed_controls':'unchanged_strict_checks','restore':'unchanged_strict_equality'},
}
CONTRACT_MATRIX['camera-gain-matrix-session/v4']={
    **CONTRACT_MATRIX['camera-gain-matrix-session/v3'],'input_source':GAIN_MATRIX_SOURCE,
}
CONTRACT_MATRIX['camera-gain-matrix-report/v4']={
    **CONTRACT_MATRIX['camera-gain-matrix-report/v3'],'input_source':GAIN_MATRIX_SOURCE,
    'initial_IFP':'exact_planned_writes_and_successful_bounded_observations_not_equality',
    'timestamp_boundary':'millisecond_equal_final_Stop_allowed_only_after_final_GetLatest_call_with_exact_one_Stop_count',
}

EXPOSURE_REBUILD_SOURCE='camera-exposure-rebuild/v1'
REBUILD_COARSE={5:list(COARSE),15:list(COARSE[:8])}
REBUILD_CORES=[528,544,1040]
REBUILD_ISP=[96,128,160]
REBUILD_COLORS=[[96,128,160,128],[128,96,128,160],[160,128,96,128],[128,128,128,128]]
CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE]={
    **CONTRACT_MATRIX[GAIN_MATRIX_SOURCE],
    'purpose':'target_specific_exposure_gain_images_FD_ABA_stream_hold_not_absolute_photometric_calibration',
    'stages':76,'blocks':[5,15],'points':74,'coarse':REBUILD_COARSE,
    'core_codes':REBUILD_CORES,'ISP_targets':REBUILD_ISP,'color_vectors':REBUILD_COLORS,
    'point_order':['reference_start','coarse_sweep','two_anchor_core_ISP_product','color_vectors','hold','FD_off_on_off','reference_end'],
    'adaptive_anchor':{'basis':'mean_of_last_two_DMA_Y_means_per_coarse_point',
        'eligible':'16<=mean<=224','select':'minimum_absolute_distance_to128_tie_lowest_index',
        'secondary':'max(0,primary_index_minus2)',
        'none_eligible':'declared_coarse128_index5_with_coverage_false_no_valid_calibration_claim'},
    'frames':{'ordinary_point':6,'hold_point':128,'FD_ABA_point':16,
        'save_indices':'0,1,n_minus2,n_minus1','DMA_per_block':[380,368],
        'DMA_total':748,'max_ROI_files':296,
        'receive':'two_explicit_linear_buffers_one_outstanding_DMA_at_most',
        'rearm':'immediately_after_completed_event_closed_before_slow_state_or_image_work',
        'buffer_ownership':'only_completed_slot_read_while_other_slot_is_pending',
        'point_boundary':'one_prefetched_next_frame_may_overlap_target_writes_first_two_images_are_transient_evidence',
        'error_cleanup':'StopCapture_then_close_pending_event_no_retry_or_reuse_as_success'},
    'controls':{'core':'one_declared_packed_code_all_four_channels','coarse':'planned_point_value_except_FD_on_experiment_bounded_1to1585_at5fps_or1to528_at15fps',
        'ISP':'planned_four_values_strict_when_FD_off_observed_1to512_when_FD_on',
        'A102':'saved_high_bits_low4_clear_or2_only_in_middle_ABA_point',
        'AWB_RGB':128,'A355_mask96':32,'AE_dgain':'per_block_baseline_then_unchanged',
        'fine_and_line_length':'per_block_baseline_then_unchanged',
        'subject_brightness':'raw_data_not_failure_on_real_flicker_or_clipping'},
    'state_registers':CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]['state_registers']+['3014'],
    'timing_snapshot_registers':['0010','0012','0014','0016','0018','300a','300c','3012','3014','3016','3024','3028','302a','302c','302e','3030','3032','3034','3036','3038','3040','3056','3058','305a','305c','32d4','32d6','32d8','32da'],
    'memory_extra':'one614400_linear_buffer_plus24576_static_CRC_scratch_no_MCU_RAM_or_ROM_patches',
    'frame_proof':'planned_writes_before_point_and_full_state_before_after_each_DMA_wait_raw_VSYNC_and_buffer_slot',
    'stream_allowed_operations':['WriteRegisterI2c','WriteMcuVariableI2c','FlushDataCache','SetReceiving','WaitFrame','IsFinishedReceiving','CloseDMAEvent','InvalidateDataCache','GetLatestVsyncTiming'],
    'post_seed_allowed_operations':['ClearBuffer','CloseDMAEvent','FlushDataCache','GetLatestVsyncTiming','InvalidateDataCache','IsBusyAfterStop','IsFinishedReceiving','SetFrameRate','SetReceiving','StartCapture','StopCapture','WaitFrame','WriteMcuVariableI2c','WriteRegisterI2c','ActivateNone'],
    'stream_write_labels':'only_registered_rb_point_nine_physical_writes_or_rb_gate_A102_for_ABA',
    'cadence':'OS_return_time_and_raw_service_timestamps_recorded_no_raw_unit_assumption_no_optical_time_claim',
    'FD_on_exposure':'record_observed_coarse_changes_no_seconds_estimate_from_requested_coarse',
    'band_profile':'16_six_row_bands_in_center128x96_YUYV_ROI_each_DMA',
    'image_proof':'four_target_requested_images_each_point_native_YUYV_CRC_SHA_first_two_and_last_two',
    'color_conversion':{'query':'GetSuitableY2rStandardCoefficient','allowed':[0,1,2,3],'proof':'successful_readonly_call_and_REBUILD_COLOR_MATRIX_value'},
    'snapshots':'before_stream_and_after_stop_each_fps_block_with_static_alias_and_non_atomic_CCM_windows',
    'refresh':{**CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]['refresh'],'max_writes':3},
    'live_sequencer':{**CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]['live_sequencer'],'max_writes':11},
    'preview_awb':{**CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]['preview_awb'],'max_writes':5},
    'awb_force_unity':{**CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]['awb_force_unity'],'max_writes':3},
    'RAM':'existing28MiB_arena_plus_existing614400_linear_buffer_and_one_extra614400_linear_buffer',
    'files':{'rebuild_points.csv':65536,'rebuild_frames.csv':524288,'rebuild_bands.csv':262144},
    'max_files':313,'startup_journal':{**CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]['startup_journal'],'expected_prefreeze_records':204},
    'local_internal_fields':CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]['local_internal_fields']+
        ['rebuild_pending_receive','rebuild_buffer_slot','rebuild_frame_ordinal','rebuild_plan','rebuild_anchor','rebuild_band_profile'],
    'calibration_validated':False,'optical_time_verified':False,'external_reference':'not_required_for_raw_evidence_collection_no_ISO_or_EV_anchor_claim',
}
CONTRACT_MATRIX['camera-exposure-rebuild-session/v1']={
    **CONTRACT_MATRIX['camera-gain-matrix-session/v4'],'input_source':EXPOSURE_REBUILD_SOURCE,'readiness_deadline_seconds':1800,
}
CONTRACT_MATRIX['camera-exposure-rebuild-report/v1']={
    'compute':False,'lane_admission':False,'override':None,'proof':'local_trace',
    'input_source':EXPOSURE_REBUILD_SOURCE,'verification':'exact74_point_plan_adaptive_anchor_from_actual_images_controls_DMA_buffer_ownership_images_bands_snapshots_restore_integrity',
    'unknown_missing_or_extra_points':'reject','calibration_validated':False,'optical_time_verified':False,
    'lighting':'unknown_and_measured_variability_retained_not_assumed_steady',
}

EXPOSURE_REBUILD_V1_SOURCE=EXPOSURE_REBUILD_SOURCE
EXPOSURE_REBUILD_SOURCE='camera-exposure-rebuild/v2'
CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE]={
    **CONTRACT_MATRIX[EXPOSURE_REBUILD_V1_SOURCE],
    'point_order':['reference_start','coarse_sweep','two_anchor_core_ISP_product','color_vectors','hold','reference_end','FD_off_on_off'],
    'FD_ABA_IFP':'all_three_transition_points_observe_successful1to512_values_not_manual_calibration_proof',
    'FD_after_intervention':{'point_kind':'FD_off_after','before_frame':8,'writes':'exactly_one_planned_four_channel128_write_each_block_unconditional',
        'proof':'REBUILD_REASSERT_begin_end_and_four_successful_calls_before_frame8_after_frame7',
        'failure':'RPC_error_stops_no_retry','not_repair':'always_executed_once_independent_of_prior_values'},
    'manual_points':'unchanged_strict_coarse_core_IFP_controls',
    'reference_bracket':'reference_end_before_FD_ABA_so_main_image_matrix_not_mixed_with_FD_intervention',
}
CONTRACT_MATRIX['camera-exposure-rebuild-session/v2']={**CONTRACT_MATRIX['camera-exposure-rebuild-session/v1'],'input_source':EXPOSURE_REBUILD_SOURCE}
CONTRACT_MATRIX['camera-exposure-rebuild-report/v2']={**CONTRACT_MATRIX['camera-exposure-rebuild-report/v1'],'input_source':EXPOSURE_REBUILD_SOURCE,
    'verification':'v1_exact_plan_and_proof_plus_unconditional_frame8_intervention_and_observed_FD_gain_segments',
    'FD_after_reassert_hold':'derived_from_all_eight_post_intervention_readbacks_not_assumed'}

EXPOSURE_RESPONSE_SOURCE='camera-exposure-response-fit/v1'
CONTRACT_MATRIX[EXPOSURE_RESPONSE_SOURCE]={
    'compute':False,'lane_admission':False,'override':None,'proof':'local_trace',
    'input_source':EXPOSURE_REBUILD_SOURCE,'input_integrity':'run_strict_rebuild_verifier_CRC_SHA_again_before_fit',
    'profiles':'independent5_and15fps_pipeline_fingerprints_no_cross_profile_merge',
    'images':'last_saved_image_per_coarse_for_training_both_last_images_for_holdout_and_gain_observations',
    'training_indices':[0,2,4,6,8,9],'holdout_indices':[1,3,5,7],
    'partition':'intersect_fixed_indices_with_profile_grid_no_result_based_resplit',
    'samples_per_channel':100,'sample_grid':[10,10],
    'encoded_domain':[4,250],'minimum_domain_span':32,'minimum_training_exposures':3,
    'training_exposure_coverage':0.8,'holdout_pixel_coverage':0.8,'minimum_holdout_exposures':2,
    'training_tail_plateau':{'upper_training_median_at_least':128,'maximum_code_rise_per_EV':2,
        'action':'mask_upper_exposure_channel_only_using_training_data_no_holdout_information'},
    'solver':{'method':'bounded_positive_log_increment_lsq_linear_bvls','smoothing':10,'min_log_increment':1e-6,'iterations':200,'tol':1e-8},
    'max_holdout_error_EV':0.25,'gain_output':'effective_RGB_exposure_shift_only_if_base_relative_response_passes',
    'gain_transfer':'two_distinct_anchor_exposures_both_valid_and_channel_shift_disagreement_at_most0.25EV',
    'pipeline_scope':'record_start_and_post_FD_endpoint_fingerprint_differences_no_assumed_stability',
    'external_reference':'preserved_approximate_not_used_in_fitting_or_holdout_correction',
    'absolute_calibration_validated':False,'optical_time_verified':False,
    'failure':'explicit_insufficient_domain_or_holdout_failure_no_unit_slope_no_fallback_no_threshold_weakening',
    'runtime_internal_fields':'none_new_offline_local_analysis_only',
}

EXPOSURE_DENSE_SOURCE='camera-exposure-dense/v1'
DENSE_COARSE={15:[32,48,64,80,96,112,128,160,192,224,256,320,384,448,528]}
DENSE_COARSE[5]=DENSE_COARSE[15]+[768,1024,1280,1585]
CONTRACT_MATRIX[EXPOSURE_DENSE_SOURCE]={
    **CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE],
    'purpose':'dense_midrange_response_identification_and_two_anchor_gain_images_no_FD_or_hold_intervention',
    'local_internal_fields':CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE]['local_internal_fields']+['rebuild_dense_mode'],
    'coarse':DENSE_COARSE,'point_order':['reference_start','dense_coarse_sweep','two_anchor_core_ISP_product','reference_end'],
    'adaptive_anchor':{'basis':'last_two_DMA_Y_means','eligible':'16<=mean<=224',
        'primary_target_Y':80,'secondary_target_Y':120,'minimum_anchor_separation_EV':0.5,
        'select':'nearest_target_tie_lowest_index_secondary_time_at_least_sqrt2_primary',
        'none_eligible':'declared_indices5_and9_coverage_false'},
    'frames':{**CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE]['frames'],
        'hold_point':0,'FD_ABA_point':0,'DMA_per_block':[234,210],'DMA_total':444},
    'controls':{**CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE]['controls'],
        'A102':'saved_high_bits_low4_clear_through_all_points','coarse':'planned_point_value',
        'ISP':'planned_four_values_strict_through_all_points'},
    'FD_ABA_IFP':'not_present','FD_after_intervention':None,
    'reference_bracket':'same128_coarse_reference_before_and_after_dense_and_gain_images',
    'live_sequencer':{**CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE]['live_sequencer'],'max_writes':7},
}
CONTRACT_MATRIX['camera-exposure-dense-session/v1']={**CONTRACT_MATRIX['camera-exposure-rebuild-session/v2'],'input_source':EXPOSURE_DENSE_SOURCE}
CONTRACT_MATRIX['camera-exposure-dense-report/v1']={**CONTRACT_MATRIX['camera-exposure-rebuild-report/v2'],'input_source':EXPOSURE_DENSE_SOURCE,
    'verification':'exact19_and15_coarse_points_74_total_two_safe_anchors_444_DMA296_images_same_buffer_state_snapshot_integrity_restore_proofs'}
EXPOSURE_DENSE_RESPONSE_SOURCE='camera-exposure-response-fit/v2'
CONTRACT_MATRIX[EXPOSURE_DENSE_RESPONSE_SOURCE]={
    **CONTRACT_MATRIX[EXPOSURE_RESPONSE_SOURCE],'input_source':EXPOSURE_DENSE_SOURCE,
    'training_indices':'even','holdout_indices':'odd','grid_counts':[15,19],
    'partition':'fixed_alternating_indices_on_new_independent_dense_capture_no_resplit_of_old_failures',
    'training_tail_plateau':{**CONTRACT_MATRIX[EXPOSURE_RESPONSE_SOURCE]['training_tail_plateau'],
        'action':'mask_both_adjacent_training_exposure_channels_when_plateau_demonstrated_no_holdout_information'},
}

EXPOSURE_LUMA_SOURCE='camera-exposure-luma-transfer/v1'
CONTRACT_MATRIX[EXPOSURE_LUMA_SOURCE]={
    'compute':False,'lane_admission':False,'override':None,'proof':'local_trace',
    'input_source':EXPOSURE_DENSE_SOURCE,
    'input_integrity':'strict_dense_report_CRC_SHA_before_any_fit_unknown_source_fails_closed',
    'purpose':'fixed_scene_center_ROI_mean_Y_to_baseline_equivalent_exposure_not_RGB_radiometry',
    'profiles':'separate5_and15fps_no_pipeline_endpoint_changes',
    'partition':'even_coarse_indices_excluding_both_late_baseline_gain_anchor_conditions',
    'holdout':'all_odd_coarse_indices_both_last_images_in_domain_plus_both_late_baseline_gain_anchors',
    'anchor_holdout':'core528_ISP128_two_distinct_collector_anchors_coverage_true_separation_at_least0.5EV',
    'images':'Y_bytes_of_last_saved_training_image_both_last_images_for_validation_and_gain',
    'encoded_Y_domain':[16,224],'minimum_training_exposures':3,'minimum_domain_span':32,
    'minimum_holdout_exposures':2,'holdout_pixel_coverage':0.8,'max_holdout_error_EV':0.25,
    'training_tail_plateau':{'lower_median_at_least':128,'maximum_code_rise_per_EV':2,
        'action':'exclude_both_training_neighbors_using_only_training_images'},
    'solver':'piecewise_linear_log2_exposure_in_ROI_mean_Y_no_extrapolation',
    'timing':'coarse_plus_fine_divided_by_line_length_relative_units_optical_time_unverified',
    'gain_output':'baseline_equivalent_luma_exposure_shift_only_after_all_base_checks_pass',
    'gain_transfer':'all_four_images_at_two_anchors_in_domain_maximum_minus_minimum_shift_at_most0.25EV',
    'external_reference':'not_used_central_only_future_protocol_old_global_values_never_relabeled',
    'failure':'explicit_reject_no_threshold_weakening_no_fallback_no_runtime_write',
    'calibration_validated':False,'optical_time_verified':False,
    'runtime_internal_fields':'none_new_offline_local_analysis_only',
}

EXPOSURE_LUMA_VALIDATE_SOURCE='camera-exposure-luma-transfer-check/v1'
CONTRACT_MATRIX[EXPOSURE_LUMA_VALIDATE_SOURCE]={
    'compute':False,'lane_admission':False,'override':None,'proof':'local_trace',
    'input_source':EXPOSURE_DENSE_SOURCE,'model_source':EXPOSURE_LUMA_SOURCE,
    'input_integrity':'strict_dense_report_CRC_SHA_and_caller_pinned_frozen_model_SHA256',
    'model':'fixed_previous_batch_knots_and_gain_shifts_never_refit_new_data',
    'illumination_offset':'only_reference_start_coarse128_core528_ISP128_last_two_images_mean_EV',
    'validation':'all_in_domain_coarse_images_reference_end_and_all_gain_images_both_last_images',
    'minimum_coarse_holdouts':2,'max_error_EV':0.25,'coverage':0.8,'extrapolation':False,
    'pipeline_match':{'flat':['3210','32c0','32c2','337c','337e']+
        [f'{x:04x}' for x in range(0x3604,0x3640,2)]+[f'{x:04x}' for x in range(0x2332,0x2348,2)]+
        [f'{x:04x}' for x in range(0xab3c,0xab62)],
        'matrix_mode':'entire_recorded_context',
        'rebuild_timing':['0010','0012','0014','0016','0018','300a','300c','3014','3016','3040']},
    'external_reference':'central_only_before_after_kept_separate_no_residual_correction',
    'failure':'pipeline_mismatch_missing_proof_unknown_source_or_holdout_failure_rejects_no_refit',
    'runtime_internal_fields':'none_new_offline_local_analysis_only',
}


CAMERA_HANDOFF_SOURCE='camera-hardware-handoff/v1'
CAMERA_HANDOFF_BOOTSTRAP_SOURCE='camera-hardware-handoff-bootstrap/v1'
CONTRACT_MATRIX[CAMERA_HANDOFF_SOURCE]={
    'compute':False,'lane_admission':False,'override':None,'proof':'local_trace',
    'boundary':'image_free_local_evidence_package_export_and_integrity_verification',
    'files':'explicit_SHA256_size_UTF8_text_allowlist_no_symlinks_or_path_traversal',
    'scope_roots':['handoff/camera-20261114','diagnostics/camera-deep-probe','diagnostics/native-trace'],
    'text_suffixes':['.py','.c','.h','.md','.csv','.json','.jsonl','.txt','.sh','.yml','.yaml'],
    'text_names':['Makefile','Makefile.probe','Makefile.recovery','Makefile.storage','Makefile.offline','LICENSE','.gitignore','.gitattributes'],
    'excluded':'all_pixel_images_archives_binaries_build_outputs_memory_dumps_and_unrelated_dirty_changes',
    'omission_proof':'original_relative_name_size_SHA256_only_does_not_prove_public_image_availability',
    'verification':'package_integrity_only_never_hardware_calibration_or_image_complete_capture_pass',
    'local_generated_exclusions':['build*','__pycache__','Film*.3dsx','Film*.elf','Film*.map','Film*.smdh'],
    'local_generated_scope':'diagnostics_only_not_handoff_evidence_and_never_allowed_manifest_entries',
    'capture_verifiers':'unchanged_missing_original_images_still_fail_closed',
    'runtime_internal_fields':'none_new_package_provenance_is_local_only',
}
CONTRACT_MATRIX[CAMERA_HANDOFF_BOOTSTRAP_SOURCE]={
    'compute':False,'lane_admission':False,'override':None,'proof':'local_trace',
    'boundary':'explicit_local_compatibility_symlink_setup_no_device_or_compute_work',
    'links':{'build/worktrees/camera-deep-probe':'diagnostics/camera-deep-probe',
             'build/hardware/native-trace':'diagnostics/native-trace'},
    'existing_path':'preserve_fail_on_conflict_no_overwrite_or_removal',
    'failure':'unknown_source_or_missing_target_reject_before_any_mutation',
    'runtime_internal_fields':'none_new',
}


def stages():
    yield "sweep_seed", "seed", None, None
    yield "sweep_ref_00", "code_baseline", None, None
    for state, (_, code, isp) in enumerate(STATES):
        for point, coarse in enumerate(COARSE):
            yield f"sweep_s{state:02d}_c{coarse:04d}", "code_baseline", state, coarse
        yield f"sweep_ref_{state+1:02d}", "code_baseline", None, None
    yield "sweep_restore", "code_baseline", None, None


def header():
    codes = ",".join(str(s[1]) for s in STATES)
    modes = ",".join("1" if s[2] == "unity" else "0" for s in STATES)
    return f'''#pragma once
/* Generated by Film3DS/tools/exposure_sweep_contract.py. Do not edit.
 * CONTRACT_MATRIX: {SOURCE}; bounded Sensor1 local diagnostic compute.
 * No Vision LaneAdmission; explicit standalone_hardware_diagnostic override.
 * Proof: per-frame before/after reads, frames, planned states and raw YUYV.
 * RAM28MiB, files<=320; raw ROI x256 y192 w128 h96. No SD writes.
 * No IPC dump, retry, hidden queue, eviction or skipped stage.
 * Exact Cartesian coverage; no code-to-photometric-gain assumption.
 * Restore all five original controls and four original ISP gains on exit.
 * ISP writes: only 32d4/32d6/32d8/32da, native observed 1..512 or unity128.
 * Capture error or readback drift stops; dark/clipped frames remain evidence.
 */
enum {{ SWEEP_VERSION=2, SWEEP_STATES={len(STATES)}, SWEEP_POINTS={len(COARSE)}, SWEEP_STAGES={MAX_STAGES} }};
#define SWEEP_CHECK_SOURCE "{CHECK_SOURCE}"
#define SWEEP_REFRESH_SOURCE "{REFRESH_SOURCE}"
#define SWEEP_MANUAL_SOURCE "{MANUAL_SOURCE}"
#define GAIN_MATRIX_SOURCE "{GAIN_MATRIX_SOURCE}"
#define EXPOSURE_REBUILD_SOURCE "{EXPOSURE_REBUILD_SOURCE}"
#define EXPOSURE_DENSE_SOURCE "{EXPOSURE_DENSE_SOURCE}"
static const unsigned dense_coarse5[]={{ {','.join(map(str,DENSE_COARSE[5]))} }};
static const unsigned dense_coarse15[]={{ {','.join(map(str,DENSE_COARSE[15]))} }};
#define REBUILD_REFRESH_WRITES 3
static const unsigned rebuild_state_registers[]={{ {','.join('0x'+a for a in CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE]['state_registers'])} }};
static const unsigned rebuild_timing_registers[]={{ {','.join('0x'+a for a in CONTRACT_MATRIX[EXPOSURE_REBUILD_SOURCE]['timing_snapshot_registers'])} }};
static const unsigned rebuild_core_codes[]={{ {','.join(map(str,REBUILD_CORES))} }};
static const unsigned rebuild_ISP_targets[]={{ {','.join(map(str,REBUILD_ISP))} }};
static const unsigned rebuild_colors[4][4]={{ {','.join('{'+','.join(map(str,row))+'}' for row in REBUILD_COLORS)} }};

#define GAIN_MATRIX_REFRESH_WRITES 9
static const unsigned gain_matrix_registers[]={{ {','.join('0x'+a for a in CONTRACT_MATRIX[GAIN_MATRIX_SOURCE]['state_registers'])} }};
static const unsigned gain_matrix_fps[]={{ {','.join(str(n['fps']) for n in GAIN_MATRIX_NODES)} }};
static const unsigned gain_matrix_gate[]={{ {','.join(str(n['gate']) for n in GAIN_MATRIX_NODES)} }};
static const unsigned gain_matrix_coarse[]={{ {','.join(str(n['coarse']) for n in GAIN_MATRIX_NODES)} }};
static const unsigned gain_matrix_targets[]={{128,96,160}};
#define CCM_BOOT_SOURCE "{CCM_BOOT_SOURCE}"
enum {{ BOOT_RECORD_CAP=768, BOOT_LINE_CAP=160, BOOT_TOTAL_CAP=2097152 }};
#define CCM_BYTES_SOURCE "{CCM_BYTES_SOURCE}"
static const unsigned ccm_bytes_mcu[]={{ {','.join('0x'+a for a in CONTRACT_MATRIX[CCM_BYTES_SOURCE]['snapshot']['mcu'])} }};
static const unsigned ccm_words[]={{ {','.join('0x'+a for a in CCM_WORDS)} }};
#define CCM_SOURCE "{CCM_SOURCE}"
static const unsigned ccm_hg_arms[]={{0,0,0}};
static const unsigned ccm_disable[]={{0,1,0}};
static const unsigned ccm_targets[]={{96,128,160,128}};
static const unsigned ccm_mcu[]={{ {','.join('0x'+a for a in CONTRACT_MATRIX[CCM_SOURCE]['snapshot']['mcu'])} }};
#define HG_SOURCE "{HG_SOURCE}"
#define HG_REFRESH_WRITES {CONTRACT_MATRIX[HG_SOURCE]['refresh']['max_writes']}
enum {{ HG_POLL_LIMIT=64, HG_POLL_SLEEP_NS=4000000 }};
static const unsigned hg_arms[]={{1,0,1}};
static const unsigned hg_delays[]={{ {','.join(map(str,HG_DELAYS))} }};
static const unsigned hg_mcu[]={{ {','.join('0x'+a for a in HG_MCU)} }};
static const unsigned hg_physical[]={{ {','.join('0x'+a for a in HG_PHYSICAL)} }};
#define ISP_UPDATE_SOURCE "{UPDATE_SOURCE}"
#define ISP_UPDATE_REFRESH_WRITES {CONTRACT_MATRIX[UPDATE_SOURCE]['refresh']['max_writes']}
#define NO_RECEIVE_SOURCE "{NO_RECEIVE_SOURCE}"
#define NO_RECEIVE_REFRESH_WRITES {CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['refresh']['max_writes']}
#define WRITER_WINDOW_SOURCE "{WRITER_WINDOW_SOURCE}"
#define WRITER_OBSERVE_SOURCE "{WRITER_OBSERVE_SOURCE}"
enum {{ WRITER_GATE_PORT={CONTRACT_MATRIX[WRITER_WINDOW_SOURCE]['gate']['port']},
        WRITER_ACCEPT_MS={CONTRACT_MATRIX[WRITER_WINDOW_SOURCE]['gate']['accept_timeout_ms']},
        WRITER_START_MS={CONTRACT_MATRIX[WRITER_WINDOW_SOURCE]['gate']['start_timeout_ms']},
        WRITER_RELEASE_MS={CONTRACT_MATRIX[WRITER_WINDOW_SOURCE]['gate']['release_timeout_ms']} }};
enum {{ NO_RECEIVE_VSYNC_COUNT={CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['no_receive']['VSYNC_observations']},
        NO_RECEIVE_RED_READS={CONTRACT_MATRIX[NO_RECEIVE_SOURCE]['no_receive']['red_reads_per_VSYNC']} }};
static const unsigned isp_update_initial[]={{ {','.join(str(v) for _,v in UPDATE_ARMS)} }};
static const unsigned isp_update_continuous[]={{ {','.join('1' if mode=='continuous' else '0' for mode,_ in UPDATE_ARMS)} }};
static const unsigned isp_update_mode[]={{ {','.join('0x'+a for a in CONTRACT_MATRIX[UPDATE_SOURCE]['observation']['mode_registers'])} }};
static const unsigned isp_update_registers[]={{ {','.join('0x'+a for a in CONTRACT_MATRIX[UPDATE_SOURCE]['observation']['registers'])} }};
enum {{ SWEEP_TRACE_PHASE=5, SWEEP_TRACE_FRAMES=8,
        SWEEP_REFRESH_POLLS={CONTRACT_MATRIX[REFRESH_SOURCE]['refresh']['poll_limit']},
        SWEEP_REFRESH_STABLE={CONTRACT_MATRIX[REFRESH_SOURCE]['refresh']['stable_samples']} }};
#define SWEEP_REFRESH_SLEEP_NS {CONTRACT_MATRIX[REFRESH_SOURCE]['refresh']['poll_sleep_ns']}
#define SWEEP_AWB_CONTROL_MASK {CONTRACT_MATRIX[REFRESH_SOURCE]['awb_force_unity']['verify_mask']}
static const unsigned sweep_trace_mcu[]={{ {','.join('0x'+a for a in CONTRACT_MATRIX[CHECK_SOURCE]['transition_probe']['mcu_registers'])} }};
static const unsigned sweep_coarse[]={{ {','.join(map(str, COARSE))} }};
static const unsigned sweep_codes[]={{ {codes} }};
static const unsigned sweep_unity[]={{ {modes} }};
'''


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--header", type=Path, required=True)
    p.add_argument("--check", action="store_true")
    a = p.parse_args()
    if a.check:
        if a.header.read_text() != header():
            raise SystemExit("C contract differs from authoritative matrix")
    else:
        a.header.write_text(header())
