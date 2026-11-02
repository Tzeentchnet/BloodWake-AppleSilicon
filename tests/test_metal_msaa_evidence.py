import contextlib
import copy
import hashlib
import io
import json
import unittest
from unittest import mock

from tools import metal_msaa_evidence as msaa


class MetalMsaaEvidenceTests(unittest.TestCase):
    def configuration(self, sample_count=4):
        return {
            "width": 2560,
            "height": 1920,
            "sample_count": sample_count,
            "color_format": "bgra8unorm",
            "depth_stencil_format": "depth24unorm_stencil8",
            "render_scale": 4,
        }

    def complete_record(self, sample_count=4):
        configuration = self.configuration(sample_count)
        costs = msaa.compute_costs(configuration)
        record = msaa.make_template(
            configuration,
            {
                "max_attachment_bytes": costs["retained_attachment_bytes"],
                "max_gpu_time_regression_percent": 10,
                "max_resolve_gpu_time_ms": 0.5,
                "min_measured_frames": 600,
            },
        )
        record["measurement"] = {
            "control": {
                "device": "synthetic-apple-gpu",
                "os": "synthetic-macos",
                "build": "fixture-build",
                "scene": "fixture-scene",
                "gpu_timer": "fixture-command-buffer-timestamps",
                "protocol": "fixed-camera-abba",
                "run_order": "baseline-msaa-msaa-baseline",
            },
            "warmup_frames": 60,
            "baseline_frame_count": 600,
            "msaa_frame_count": 600,
            "gpu_time_ms": {
                "baseline": {"p50": 1.0, "p95": 2.0, "p99": 3.0},
                "msaa": {"p50": 1.1, "p95": 2.2, "p99": 3.3},
                "resolve_p95": 0.5,
            },
            "peak_attachment_bytes": costs["retained_attachment_bytes"],
        }
        record["correctness"] = {
            check: True for check in msaa.CORRECTNESS_CHECKS
        }
        for index, crop in enumerate(record["visual"]["crops"]):
            feature = crop["feature"]
            crop.update(
                {
                    "baseline_sha256": hashlib.sha256(
                        ("baseline-" + feature).encode()
                    ).hexdigest(),
                    "msaa_sha256": hashlib.sha256(
                        ("msaa-" + feature).encode()
                    ).hexdigest(),
                    "correct": True,
                    "improved": index == 0,
                }
            )
        record["visual"]["reviewer"] = "fixture-reviewer"
        record["visual"]["visible_benefit_beyond_render_scale"] = True
        return record

    def test_valid_2x_4x_8x_costs_and_records(self):
        expected = {
            2: (78_643_200, 117_964_800, 58_982_400),
            4: (157_286_400, 196_608_000, 98_304_000),
            8: (314_572_800, 353_894_400, 176_947_200),
        }
        for samples, (
            incremental,
            retained,
            resolve_traffic,
        ) in expected.items():
            with self.subTest(samples=samples):
                costs = msaa.compute_costs(self.configuration(samples))
                self.assertEqual(
                    costs["incremental_multisample_attachments_bytes"], incremental
                )
                self.assertEqual(costs["retained_attachment_bytes"], retained)
                self.assertEqual(
                    costs["minimum_resolve_traffic_bytes"], resolve_traffic
                )
                report = msaa.evaluate_record(self.complete_record(samples))
                self.assertEqual(report["decision"]["verdict"], "go")

    def test_resolution_sample_count_format_and_scale_are_strict(self):
        invalid = (
            ("width", 0),
            ("width", -1),
            ("width", True),
            ("width", 1.0),
            ("width", float("nan")),
            ("width", msaa.MAX_TEXTURE_DIMENSION + 1),
            ("height", 0),
            ("height", float("inf")),
            ("sample_count", 1),
            ("sample_count", 3),
            ("sample_count", 16),
            ("sample_count", False),
            ("color_format", "rgba8unorm"),
            ("color_format", "depth24unorm_stencil8"),
            ("depth_stencil_format", "bgra8unorm"),
            ("depth_stencil_format", "depth32float_stencil8"),
            ("render_scale", 3),
        )
        for field, value in invalid:
            with self.subTest(field=field, value=value):
                configuration = self.configuration()
                configuration[field] = value
                with self.assertRaises(msaa.EvidenceError):
                    msaa.compute_costs(configuration)

        configuration = self.configuration()
        configuration["unexpected"] = 1
        with self.assertRaisesRegex(msaa.EvidenceError, "unknown fields"):
            msaa.compute_costs(configuration)

    def test_non_finite_and_non_json_numbers_are_rejected(self):
        for value in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(value=value):
                thresholds = self.complete_record()["thresholds"]
                thresholds["max_gpu_time_regression_percent"] = value
                with self.assertRaisesRegex(msaa.EvidenceError, "finite"):
                    msaa.validate_thresholds(thresholds)

                record = self.complete_record()
                record["measurement"]["gpu_time_ms"]["msaa"]["p95"] = value
                with self.assertRaisesRegex(msaa.EvidenceError, "finite"):
                    msaa.evaluate_record(record)

        for token in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(token=token), self.assertRaisesRegex(
                msaa.EvidenceError, "non-finite"
            ):
                msaa.loads_record('{"value": ' + token + "}")

    def test_checked_memory_arithmetic_rejects_overflow(self):
        self.assertEqual(msaa.checked_add_u64(msaa.UINT64_MAX, 0), msaa.UINT64_MAX)
        self.assertEqual(msaa.checked_mul_u64(msaa.UINT64_MAX, 1), msaa.UINT64_MAX)
        with self.assertRaisesRegex(msaa.EvidenceError, "overflow"):
            msaa.checked_add_u64(msaa.UINT64_MAX, 1)
        with self.assertRaisesRegex(msaa.EvidenceError, "overflow"):
            msaa.checked_mul_u64(msaa.UINT64_MAX, 2)
        with self.assertRaises(msaa.EvidenceError):
            msaa.checked_mul_u64(1 << 64, 1)

    def test_missing_evidence_is_a_closed_no_go(self):
        configuration = self.configuration()
        costs = msaa.compute_costs(configuration)
        template = msaa.make_template(
            configuration,
            {
                "max_attachment_bytes": costs["retained_attachment_bytes"],
                "max_gpu_time_regression_percent": 10,
                "max_resolve_gpu_time_ms": 1,
                "min_measured_frames": 600,
            },
        )
        report = msaa.evaluate_record(template)
        self.assertFalse(report["decision"]["go"])
        reasons = "\n".join(report["decision"]["reasons"])
        self.assertIn("measurement.control.device evidence is missing", reasons)
        self.assertIn("correctness.color_resolve evidence is missing", reasons)
        self.assertIn("visual.reviewer evidence is missing", reasons)

        missing_sections = copy.deepcopy(template)
        del missing_sections["measurement"]
        del missing_sections["correctness"]
        del missing_sections["visual"]
        report = msaa.evaluate_record(missing_sections)
        self.assertEqual(report["decision"]["verdict"], "no-go")
        self.assertIn("measurement evidence is missing", report["decision"]["reasons"])
        self.assertIn("correctness evidence is missing", report["decision"]["reasons"])
        self.assertIn("visual evidence is missing", report["decision"]["reasons"])

    def test_threshold_equalities_pass_and_excesses_fail(self):
        boundary = self.complete_record()
        report = msaa.evaluate_record(boundary)
        self.assertTrue(report["decision"]["go"], report["decision"]["reasons"])
        self.assertEqual(
            report["observed"]["gpu_time_regression_percent"],
            {"p50": 10, "p95": 10, "p99": 10},
        )

        cases = {}
        cases["computed attachment"] = copy.deepcopy(boundary)
        cases["computed attachment"]["thresholds"]["max_attachment_bytes"] -= 1
        cases["measured attachment"] = copy.deepcopy(boundary)
        cases["measured attachment"]["measurement"]["peak_attachment_bytes"] += 1
        cases["gpu regression"] = copy.deepcopy(boundary)
        cases["gpu regression"]["measurement"]["gpu_time_ms"]["msaa"]["p99"] = 3.300001
        cases["resolve time"] = copy.deepcopy(boundary)
        cases["resolve time"]["measurement"]["gpu_time_ms"]["resolve_p95"] = 0.500001
        cases["frame count"] = copy.deepcopy(boundary)
        cases["frame count"]["measurement"]["msaa_frame_count"] = 599

        for name, record in cases.items():
            with self.subTest(name=name):
                report = msaa.evaluate_record(record)
                self.assertFalse(report["decision"]["go"])
                self.assertTrue(report["decision"]["reasons"])

    def test_inconsistent_or_failed_evidence_is_no_go(self):
        cases = {}
        cases["nonmonotonic"] = self.complete_record()
        cases["nonmonotonic"]["measurement"]["gpu_time_ms"]["msaa"] = {
            "p50": 3,
            "p95": 2,
            "p99": 4,
        }
        cases["peak below lower bound"] = self.complete_record()
        cases["peak below lower bound"]["measurement"]["peak_attachment_bytes"] -= 1
        cases["correctness"] = self.complete_record()
        cases["correctness"]["correctness"]["partial_clears"] = False
        cases["visual benefit"] = self.complete_record()
        cases["visual benefit"]["visual"][
            "visible_benefit_beyond_render_scale"
        ] = False
        cases["no improved crop"] = self.complete_record()
        for crop in cases["no improved crop"]["visual"]["crops"]:
            crop["improved"] = False
        cases["identical improved crop"] = self.complete_record()
        crop = cases["identical improved crop"]["visual"]["crops"][0]
        crop["msaa_sha256"] = crop["baseline_sha256"]

        for name, record in cases.items():
            with self.subTest(name=name):
                self.assertFalse(msaa.evaluate_record(record)["decision"]["go"])

    def test_stale_costs_duplicate_fields_and_bad_digests_are_rejected(self):
        stale = self.complete_record()
        stale["estimated_costs"]["retained_attachment_bytes"] += 1
        with self.assertRaisesRegex(msaa.EvidenceError, "does not match"):
            msaa.evaluate_record(stale)

        with self.assertRaisesRegex(msaa.EvidenceError, "duplicate JSON field"):
            msaa.loads_record('{"schema": 1, "schema": 2}')

        bad_digest = self.complete_record()
        bad_digest["visual"]["crops"][0]["baseline_sha256"] = "not-a-digest"
        with self.assertRaisesRegex(msaa.EvidenceError, "SHA-256"):
            msaa.evaluate_record(bad_digest)

    def test_cli_produces_machine_readable_template_and_no_go_report(self):
        args = [
            "template",
            "--width",
            "2560",
            "--height",
            "1920",
            "--sample-count",
            "4",
            "--color-format",
            "bgra8unorm",
            "--depth-stencil-format",
            "depth24unorm_stencil8",
            "--render-scale",
            "4",
            "--max-attachment-bytes",
            "196608000",
            "--max-gpu-time-regression-percent",
            "10",
            "--max-resolve-gpu-time-ms",
            "1",
            "--min-measured-frames",
            "600",
        ]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(msaa.main(args), 0)
        template = json.loads(output.getvalue())
        self.assertEqual(template["schema"], msaa.SCHEMA)
        self.assertEqual(
            template["estimated_costs"]["retained_attachment_bytes"], 196_608_000
        )

        output = io.StringIO()
        with mock.patch.object(msaa.sys, "stdin", io.StringIO(json.dumps(template))):
            with contextlib.redirect_stdout(output):
                self.assertEqual(msaa.main(["evaluate", "-"]), 1)
        report = json.loads(output.getvalue())
        self.assertEqual(report["decision"]["verdict"], "no-go")


if __name__ == "__main__":
    unittest.main()
