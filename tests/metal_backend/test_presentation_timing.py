#!/usr/bin/env python3

import json
import os
from pathlib import Path
import subprocess
import sys


SCHEMA = "bloodwake.metal-presentation-timing/v1"
EVENT_FIELDS = {
    "schema",
    "record",
    "sequence",
    "frame",
    "status",
    "direct_presenter",
    "flip_stall_ns",
    "flip_release_ns",
    "flip_hold_ns",
    "drawable_status",
    "drawable_acquire_start_ns",
    "drawable_acquire_end_ns",
    "present_request_ns",
    "command_commit_ns",
    "command_complete_ns",
    "command_status",
    "actual_present_ns",
    "actual_callback_ns",
    "actual_present_status",
    "actual_present_interval_ns",
    "display_target_status",
    "display_target_ns",
}
SUMMARY_FIELDS = {
    "schema",
    "record",
    "enabled",
    "event_capacity",
    "events_seen",
    "events_recorded",
    "events_dropped",
    "flip_releases",
    "drawables_acquired",
    "drawables_unavailable",
    "present_requests",
    "commands_completed",
    "commands_failed",
    "actual_presented",
    "actual_unavailable",
    "actual_invalid",
    "display_targets_available",
    "display_targets_unavailable",
    "actual_interval_count",
    "actual_interval_min_ns",
    "actual_interval_max_ns",
    "ordering_failures",
    "callback_timeouts",
    "allocation_failures",
    "initialization_failed",
}
PROHIBITED = (
    "descriptor",
    "combiner",
    "function_constants",
    "retail",
    "texture_identity",
    "address",
    "key_hash",
)


def run_case_output(executable, case):
    environment = os.environ.copy()
    environment.pop("BW_METAL_FRAME_TIMING", None)
    environment.pop("BW_GPU_AUDIT", None)
    result = subprocess.run(
        [str(executable), case],
        env=environment,
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
    reports = [
        json.loads(line)
        for line in result.stderr.splitlines()
        if line.startswith('{"schema":"' + SCHEMA + '"')
    ]
    for report in reports:
        fields = EVENT_FIELDS if report["record"] == "event" else SUMMARY_FIELDS
        assert set(report) == fields
        serialized = json.dumps(report, sort_keys=True)
        for field in PROHIBITED:
            assert field not in serialized
    return reports, result.stderr


def run_case(executable, case):
    return run_case_output(executable, case)[0]


def summary(reports):
    summaries = [report for report in reports if report["record"] == "summary"]
    assert len(summaries) == 1
    return summaries[0]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <backend-test-executable>")
    executable = Path(sys.argv[1]).resolve(strict=True)

    assert run_case(executable, "presentation-timing-disabled") == []

    timing_reports, timing_output = run_case_output(
        executable, "timing-percentile-report"
    )
    assert timing_reports == []
    assert (
        "[GPU-METAL] frame timing percentile test: 100 intervals, "
        "19.80 fps average, 51.00 ms median, 95.00 ms p95, "
        "99.00 ms p99, 100.00 ms max"
    ) in timing_output
    assert (
        "[GPU-METAL] GPU command timing percentile test: 100 frames, "
        "50.50 ms mean, 51.00 ms median, 95.00 ms p95, "
        "99.00 ms p99, 100.00 ms max"
    ) in timing_output

    lifecycle = summary(run_case(executable, "presentation-timing-lifecycle"))
    assert lifecycle["events_recorded"] == 1
    assert lifecycle["flip_releases"] == 1
    assert lifecycle["drawables_acquired"] == 0

    streamed = run_case(executable, "presentation-timing-stream")
    assert len(streamed) == 1
    assert streamed[0]["record"] == "event"
    assert streamed[0]["sequence"] == 0

    paced = summary(
        run_case(executable, "presentation-timing-paced-release")
    )
    assert paced["events_recorded"] == 2
    assert paced["flip_releases"] == 2
    assert paced["ordering_failures"] == 0

    report, _ = run_case_output(
        executable, "presentation-timing-report"
    )
    assert len(report) == 2
    assert report[0]["status"] == "presented"
    assert report[0]["display_target_status"] == (
        "unavailable_explicit_presenter"
    )
    assert report[0]["display_target_ns"] is None

    fractional = summary(
        run_case(executable, "presentation-timing-fractional")
    )
    assert fractional["actual_interval_count"] == 2
    assert fractional["actual_interval_min_ns"] == 16666667
    assert fractional["actual_interval_max_ns"] == 16683334

    failures = summary(run_case(executable, "presentation-timing-failure"))
    assert failures["drawables_unavailable"] == 1
    assert failures["actual_unavailable"] == 1
    assert failures["actual_invalid"] == 1

    bounded = run_case(executable, "presentation-timing-bounded")
    assert len([report for report in bounded if report["record"] == "event"]) == 512
    bounded_summary = summary(bounded)
    assert bounded_summary["events_seen"] == 515
    assert bounded_summary["events_recorded"] == 512
    assert bounded_summary["events_dropped"] == 3

    allocation = summary(
        run_case(executable, "presentation-timing-allocation-failure")
    )
    assert allocation["events_seen"] == 1
    assert allocation["events_recorded"] == 0
    assert allocation["allocation_failures"] == 1

    stopped = summary(run_case(executable, "presentation-timing-stop"))
    assert stopped["commands_completed"] == 1
    assert stopped["actual_presented"] == 1
    assert stopped["callback_timeouts"] == 0

    restart = run_case(executable, "presentation-timing-restart")
    restart_summaries = [
        report for report in restart if report["record"] == "summary"
    ]
    assert len(restart_summaries) == 2
    assert restart_summaries[0]["events_recorded"] == 1
    assert restart_summaries[1]["events_recorded"] == 0


if __name__ == "__main__":
    main()
