#!/usr/bin/env python3

import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile


SCHEMA = "bloodwake.metal-pipeline-warmup/v1"
ASYNC_SCHEMA = "bloodwake.metal-pipeline-async/v1"
HEADER_BYTES = 176
TRACE_ENTRY_BYTES = 228


def run_case(executable, case, include_async=False, **environment):
    child_environment = os.environ.copy()
    child_environment.pop("BW_METAL_PIPELINE_WARMUP", None)
    child_environment.pop("BW_METAL_PIPELINE_TRACE_OUTPUT", None)
    child_environment["BW_METAL_PIPELINE_TIMING"] = "1"
    child_environment.update(environment)
    result = subprocess.run(
        [str(executable), case],
        env=child_environment,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    if result.returncode:
        raise AssertionError(
            f"{case} failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    reports = []
    async_reports = []
    for line in result.stderr.splitlines():
        if line.startswith('{"schema":"' + SCHEMA + '"'):
            reports.append(json.loads(line))
        elif line.startswith('{"schema":"' + ASYNC_SCHEMA + '"'):
            async_reports.append(json.loads(line))
    prohibited = (
        "path",
        "descriptor",
        "combiner",
        "function_constants",
        "retail",
        "texture_identity",
        "address",
        "key_hash",
    )
    for report in reports:
        serialized = json.dumps(report, sort_keys=True)
        for field in prohibited:
            assert field not in serialized
    for report in async_reports:
        serialized = json.dumps(report, sort_keys=True)
        for field in prohibited:
            assert field not in serialized
    return (reports, async_reports) if include_async else reports


def write_variant(source, destination, mutator):
    data = bytearray(source.read_bytes())
    mutator(data)
    destination.write_bytes(data)
    destination.chmod(0o600)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <backend-test-executable>")
    executable = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="bw-metal-warmup-") as directory:
        root = Path(directory)
        root.chmod(0o700)
        artifact = root / "trace.bwmpa"

        capture = run_case(
            executable,
            "pipeline-warmup-capture",
            BW_METAL_PIPELINE_TRACE_OUTPUT=str(artifact),
        )
        assert artifact.is_file()
        assert stat.S_IMODE(artifact.stat().st_mode) == 0o600
        assert artifact.stat().st_size == HEADER_BYTES + 2 * TRACE_ENTRY_BYTES
        assert capture
        assert set(capture[-1]) == {
            "schema",
            "record",
            "status",
            "entries",
            "entries_dropped",
            "write_attempts",
            "write_successes",
            "write_failures",
            "final",
        }
        assert capture[-1]["status"] == "compatible"
        assert capture[-1]["entries"] == 2
        assert capture[-1]["final"] is True

        load = run_case(
            executable,
            "pipeline-warmup-load",
            BW_METAL_PIPELINE_WARMUP=str(artifact),
        )
        assert load == [{
            "schema": SCHEMA,
            "record": "load",
            "artifact_status": "compatible",
            "payload_status": "compatible",
            "entries": 2,
            "preloaded": 2,
            "runtime_fallback": False,
        }]

        async_keys = {
            "schema",
            "record",
            "requested",
            "enabled",
            "entry_capacity",
            "entries",
            "pending",
            "ready",
            "failed_retained",
            "scheduled",
            "coalesced_misses",
            "generic_fallback_draws",
            "installed",
            "function_failures",
            "pipeline_failures",
            "allocation_failures",
            "capacity_fallbacks",
            "function_timed",
            "function_total_ns",
            "function_max_ns",
            "pipeline_timed",
            "pipeline_total_ns",
            "pipeline_max_ns",
            "timing_unavailable",
            "timing_non_monotonic",
            "initialization_failed",
            "final",
        }

        def run_async(case):
            warmup, reports = run_case(
                executable,
                case,
                include_async=True,
                BW_METAL_PIPELINE_WARMUP=str(artifact),
            )
            assert warmup
            assert warmup[0]["payload_status"] == "compatible"
            assert reports
            for report in reports:
                assert set(report) == async_keys
                assert report["requested"] is True
                assert report["enabled"] is True
                assert report["entry_capacity"] == 32
                assert report["initialization_failed"] is False
            assert reports[-1]["final"] is True
            return reports

        report_only = run_async("pipeline-async-report")
        assert report_only[-1]["scheduled"] == 0
        assert report_only[-1]["entries"] == 0

        success = run_async("pipeline-async-success")[-1]
        assert success["scheduled"] == 1
        assert success["installed"] == 1
        assert success["function_timed"] == 1
        assert success["pipeline_timed"] == 1
        assert success["generic_fallback_draws"] >= 1

        coalesced = run_async("pipeline-async-coalescing")[-1]
        assert coalesced["scheduled"] == 1
        assert coalesced["installed"] == 1
        assert coalesced["coalesced_misses"] >= 1
        assert coalesced["generic_fallback_draws"] >= 2

        allocation = run_async(
            "pipeline-async-allocation-failure"
        )[-1]
        assert allocation["allocation_failures"] == 1
        assert allocation["failed_retained"] == 1
        assert allocation["installed"] == 0

        function = run_async("pipeline-async-function-failure")[-1]
        assert function["function_failures"] == 1
        assert function["failed_retained"] == 1
        assert function["installed"] == 0

        pipeline = run_async("pipeline-async-pipeline-failure")[-1]
        assert pipeline["pipeline_failures"] == 1
        assert pipeline["failed_retained"] == 1
        assert pipeline["installed"] == 0

        bounded_async = run_async("pipeline-async-bounded")[-1]
        assert bounded_async["scheduled"] == 32
        assert bounded_async["installed"] == 32
        assert bounded_async["capacity_fallbacks"] == 1
        assert bounded_async["entries"] == 0

        stopped = run_async("pipeline-async-stop")[-1]
        assert stopped["scheduled"] == 1
        assert stopped["installed"] == 1
        assert stopped["pending"] == 0
        assert stopped["entries"] == 0

        _, restart_reports = run_case(
            executable,
            "pipeline-async-restart",
            include_async=True,
            BW_METAL_PIPELINE_WARMUP=str(artifact),
        )
        restart_finals = [
            report for report in restart_reports if report["final"]
        ]
        assert len(restart_finals) == 2
        assert restart_finals[0]["scheduled"] == 1
        assert restart_finals[0]["pending"] == 0
        assert restart_finals[1]["scheduled"] == 0
        assert restart_finals[1]["entries"] == 0

        missing = run_case(
            executable,
            "pipeline-warmup-missing",
            BW_METAL_PIPELINE_WARMUP=str(root / "missing.bwmpa"),
        )
        assert missing[0]["artifact_status"] == "missing"
        assert missing[0]["runtime_fallback"] is True

        corrupt = root / "corrupt.bwmpa"
        write_variant(
            artifact,
            corrupt,
            lambda data: data.__setitem__(HEADER_BYTES + 20,
                                          data[HEADER_BYTES + 20] ^ 1),
        )
        corrupt_reports = run_case(
            executable,
            "pipeline-warmup-corrupt",
            BW_METAL_PIPELINE_WARMUP=str(corrupt),
        )
        assert corrupt_reports[0]["artifact_status"] == "corrupt"

        stale = root / "stale.bwmpa"
        write_variant(
            artifact,
            stale,
            lambda data: data.__setitem__(64, data[64] ^ 1),
        )
        stale_reports = run_case(
            executable,
            "pipeline-warmup-stale",
            BW_METAL_PIPELINE_WARMUP=str(stale),
        )
        assert stale_reports[0]["artifact_status"] == "stale"

        invalid_key = root / "invalid-key.bwmpa"

        def invalidate_key(data):
            struct.pack_into("<I", data, HEADER_BYTES + 8, 2)
            data[136:168] = hashlib.sha256(data[HEADER_BYTES:]).digest()

        write_variant(artifact, invalid_key, invalidate_key)
        invalid_reports = run_case(
            executable,
            "pipeline-warmup-invalid-key",
            BW_METAL_PIPELINE_WARMUP=str(invalid_key),
        )
        assert invalid_reports[0]["artifact_status"] == "compatible"
        assert invalid_reports[0]["payload_status"] == "invalid_key"
        assert invalid_reports[0]["runtime_fallback"] is True

        failed_reports = run_case(
            executable,
            "pipeline-warmup-pipeline-failure",
            BW_METAL_PIPELINE_WARMUP=str(artifact),
        )
        assert failed_reports[0]["artifact_status"] == "compatible"
        assert failed_reports[0]["payload_status"] == "pipeline_failed"
        assert failed_reports[0]["preloaded"] == 0

        public_directory = root / "public"
        public_directory.mkdir(mode=0o755)
        public_directory.chmod(0o755)
        failure = run_case(
            executable,
            "pipeline-warmup-capture-failure",
            BW_METAL_PIPELINE_TRACE_OUTPUT=str(
                public_directory / "trace.bwmpa"
            ),
        )
        assert failure[-1]["status"] == "policy_rejected"
        assert failure[-1]["write_failures"] == 1

        bounded = root / "bounded.bwmpa"
        bounded_reports = run_case(
            executable,
            "pipeline-warmup-capture-bounded",
            BW_METAL_PIPELINE_TRACE_OUTPUT=str(bounded),
        )
        assert bounded_reports[-1]["entries"] == 128
        assert bounded_reports[-1]["entries_dropped"] >= 1
        assert bounded.stat().st_size == (
            HEADER_BYTES + 128 * TRACE_ENTRY_BYTES
        )


if __name__ == "__main__":
    main()
