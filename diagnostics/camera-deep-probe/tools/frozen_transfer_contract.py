"""Registered read-only frozen-RAM transport recovery; never compute work."""
CONTRACT_MATRIX = {
    "frozen_ram_http_range": {
        "creates_compute": False, "requires_lane_admission": False,
        "override_allowed": False, "proof_location": "local_trace",
        "policy": "frozen-resume-v1", "max_attempts_per_range": 2,
        "max_transient_failures_total": 16, "max_chunk_bytes": 4096,
        "snapshot_fingerprint_recheck_before_repeat": True,
        "measurement_retries": 0,
        "recoverable_exceptions": (ConnectionResetError, TimeoutError),
    }
}
CONTRACT_MATRIX["frozen_ram_http_range_v2"] = {
    **CONTRACT_MATRIX["frozen_ram_http_range"],
    "policy": "frozen-resume-v2", "max_attempts_per_range": 4,
    "max_transient_failures_total": 32, "max_chunk_bytes": 16384,
}


def require_policy(name, chunk):
    rules=[r for r in CONTRACT_MATRIX.values() if r["policy"]==name]
    if len(rules)!=1 or not 1 <= chunk <= rules[0]["max_chunk_bytes"]:
        raise ValueError("unknown/widened frozen transfer recovery contract")
    return rules[0]
