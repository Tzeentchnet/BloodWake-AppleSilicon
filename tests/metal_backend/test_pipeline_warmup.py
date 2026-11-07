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
HEADER_BYTES = 176
TRACE_ENTRY_BYTES = 228


def run_case(executable, case, **environment):
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
    for line in result.stderr.splitlines():
        if line.startswith('{"schema":"' + SCHEMA + '"'):
            reports.append(json.loads(line))
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
    return reports


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
