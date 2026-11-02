#!/usr/bin/env python3
"""Create and evaluate evidence records for a future Metal MSAA decision."""

import argparse
from decimal import Decimal
import json
import math
from pathlib import Path
import re
import sys


SCHEMA = "bloodwake.metal-msaa-evidence/v1"
REPORT_SCHEMA = "bloodwake.metal-msaa-evaluation/v1"
UINT64_MAX = (1 << 64) - 1
MAX_TEXTURE_DIMENSION = 16384
MAX_RECORD_BYTES = 1024 * 1024
SAMPLE_COUNTS = (2, 4, 8)
RENDER_SCALES = (1, 2, 4)
COLOR_FORMATS = {"bgra8unorm": 4}
DEPTH_STENCIL_FORMATS = {"depth24unorm_stencil8": 4}
PERCENTILES = ("p50", "p95", "p99")
CONTROL_FIELDS = (
    "device",
    "os",
    "build",
    "scene",
    "gpu_timer",
    "protocol",
    "run_order",
)
CORRECTNESS_CHECKS = (
    "color_resolve",
    "depth_stencil",
    "target_switches",
    "partial_clears",
    "guest_seed_and_readback",
    "resize_and_scale_changes",
    "guest_aa_and_quincunx",
)
VISUAL_FEATURES = (
    "hulls",
    "rigging",
    "shorelines",
    "particles",
    "hud",
    "alpha_tested_edges",
)
SHA256 = re.compile(r"^[0-9a-f]{64}$")
COST_FIELDS = (
    "pixel_count",
    "color_bytes_per_sample",
    "depth_stencil_bytes_per_sample",
    "single_sample_color_bytes",
    "single_sample_depth_stencil_bytes",
    "authoritative_single_sample_attachments_bytes",
    "multisample_color_bytes",
    "multisample_depth_stencil_bytes",
    "incremental_multisample_attachments_bytes",
    "retained_attachment_bytes",
    "authoritative_resolve_target_bytes",
    "minimum_resolve_read_bytes",
    "minimum_resolve_write_bytes",
    "minimum_resolve_traffic_bytes",
)


class EvidenceError(ValueError):
    """The record cannot be evaluated safely."""


def _object(value, path):
    if not isinstance(value, dict):
        raise EvidenceError(f"{path} must be an object")
    return value


def _known_keys(value, allowed, path):
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        raise EvidenceError(f"{path} has unknown fields: {', '.join(unknown)}")


def _required(value, fields, path):
    missing = [field for field in fields if field not in value]
    if missing:
        raise EvidenceError(f"{path} is missing fields: {', '.join(missing)}")


def _uint(value, path, *, minimum=0, maximum=UINT64_MAX):
    if isinstance(value, bool) or not isinstance(value, int):
        raise EvidenceError(f"{path} must be an integer")
    if value < minimum or value > maximum:
        raise EvidenceError(f"{path} must be between {minimum} and {maximum}")
    return value


def _finite_number(value, path, *, minimum=0):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise EvidenceError(f"{path} must be a number")
    if isinstance(value, float) and not math.isfinite(value):
        raise EvidenceError(f"{path} must be finite")
    if value < minimum:
        raise EvidenceError(f"{path} must be at least {minimum}")
    return value


def _text(value, path):
    if not isinstance(value, str):
        raise EvidenceError(f"{path} must be a string")
    if not value or len(value) > 256 or any(ord(char) < 32 for char in value):
        raise EvidenceError(f"{path} must be a non-empty string without control characters")
    return value


def checked_add_u64(*values):
    total = 0
    for index, value in enumerate(values):
        value = _uint(value, f"addition operand {index}")
        if value > UINT64_MAX - total:
            raise EvidenceError("uint64 overflow while adding memory costs")
        total += value
    return total


def checked_mul_u64(*values):
    total = 1
    for index, value in enumerate(values):
        value = _uint(value, f"multiplication operand {index}")
        if value and total > UINT64_MAX // value:
            raise EvidenceError("uint64 overflow while multiplying memory costs")
        total *= value
    return total


def validate_configuration(configuration):
    configuration = _object(configuration, "configuration")
    fields = (
        "width",
        "height",
        "sample_count",
        "color_format",
        "depth_stencil_format",
        "render_scale",
    )
    _known_keys(configuration, fields, "configuration")
    _required(configuration, fields, "configuration")

    width = _uint(
        configuration["width"],
        "configuration.width",
        minimum=1,
        maximum=MAX_TEXTURE_DIMENSION,
    )
    height = _uint(
        configuration["height"],
        "configuration.height",
        minimum=1,
        maximum=MAX_TEXTURE_DIMENSION,
    )
    sample_count = _uint(configuration["sample_count"], "configuration.sample_count")
    if sample_count not in SAMPLE_COUNTS:
        raise EvidenceError(
            "configuration.sample_count must be one of "
            + ", ".join(str(value) for value in SAMPLE_COUNTS)
        )
    color_format = configuration["color_format"]
    if not isinstance(color_format, str) or color_format not in COLOR_FORMATS:
        raise EvidenceError(
            "configuration.color_format must be one of "
            + ", ".join(sorted(COLOR_FORMATS))
        )
    depth_format = configuration["depth_stencil_format"]
    if not isinstance(depth_format, str) or depth_format not in DEPTH_STENCIL_FORMATS:
        raise EvidenceError(
            "configuration.depth_stencil_format must be one of "
            + ", ".join(sorted(DEPTH_STENCIL_FORMATS))
        )
    render_scale = _uint(configuration["render_scale"], "configuration.render_scale")
    if render_scale not in RENDER_SCALES:
        raise EvidenceError(
            "configuration.render_scale must be one of "
            + ", ".join(str(value) for value in RENDER_SCALES)
        )
    return {
        "width": width,
        "height": height,
        "sample_count": sample_count,
        "color_format": color_format,
        "depth_stencil_format": depth_format,
        "render_scale": render_scale,
    }


def validate_thresholds(thresholds):
    thresholds = _object(thresholds, "thresholds")
    fields = (
        "max_attachment_bytes",
        "max_gpu_time_regression_percent",
        "max_resolve_gpu_time_ms",
        "min_measured_frames",
    )
    _known_keys(thresholds, fields, "thresholds")
    _required(thresholds, fields, "thresholds")
    return {
        "max_attachment_bytes": _uint(
            thresholds["max_attachment_bytes"], "thresholds.max_attachment_bytes"
        ),
        "max_gpu_time_regression_percent": _finite_number(
            thresholds["max_gpu_time_regression_percent"],
            "thresholds.max_gpu_time_regression_percent",
        ),
        "max_resolve_gpu_time_ms": _finite_number(
            thresholds["max_resolve_gpu_time_ms"],
            "thresholds.max_resolve_gpu_time_ms",
        ),
        "min_measured_frames": _uint(
            thresholds["min_measured_frames"],
            "thresholds.min_measured_frames",
            minimum=1,
        ),
    }


def compute_costs(configuration):
    configuration = validate_configuration(configuration)
    pixels = checked_mul_u64(configuration["width"], configuration["height"])
    color_bpp = COLOR_FORMATS[configuration["color_format"]]
    depth_bpp = DEPTH_STENCIL_FORMATS[configuration["depth_stencil_format"]]
    samples = configuration["sample_count"]

    single_color = checked_mul_u64(pixels, color_bpp)
    single_depth = checked_mul_u64(pixels, depth_bpp)
    authoritative = checked_add_u64(single_color, single_depth)
    multisample_color = checked_mul_u64(single_color, samples)
    multisample_depth = checked_mul_u64(single_depth, samples)
    multisample = checked_add_u64(multisample_color, multisample_depth)
    retained = checked_add_u64(authoritative, multisample)
    resolve_read = multisample_color
    resolve_write = single_color
    resolve_traffic = checked_add_u64(resolve_read, resolve_write)
    return {
        "pixel_count": pixels,
        "color_bytes_per_sample": color_bpp,
        "depth_stencil_bytes_per_sample": depth_bpp,
        "single_sample_color_bytes": single_color,
        "single_sample_depth_stencil_bytes": single_depth,
        "authoritative_single_sample_attachments_bytes": authoritative,
        "multisample_color_bytes": multisample_color,
        "multisample_depth_stencil_bytes": multisample_depth,
        "incremental_multisample_attachments_bytes": multisample,
        "retained_attachment_bytes": retained,
        "authoritative_resolve_target_bytes": single_color,
        "minimum_resolve_read_bytes": resolve_read,
        "minimum_resolve_write_bytes": resolve_write,
        "minimum_resolve_traffic_bytes": resolve_traffic,
    }


def make_template(configuration, thresholds):
    configuration = validate_configuration(configuration)
    thresholds = validate_thresholds(thresholds)
    return {
        "schema": SCHEMA,
        "configuration": configuration,
        "estimated_costs": compute_costs(configuration),
        "thresholds": thresholds,
        "measurement": {
            "control": {field: None for field in CONTROL_FIELDS},
            "warmup_frames": None,
            "baseline_frame_count": None,
            "msaa_frame_count": None,
            "gpu_time_ms": {
                "baseline": {percentile: None for percentile in PERCENTILES},
                "msaa": {percentile: None for percentile in PERCENTILES},
                "resolve_p95": None,
            },
            "peak_attachment_bytes": None,
        },
        "correctness": {check: None for check in CORRECTNESS_CHECKS},
        "visual": {
            "reviewer": None,
            "visible_benefit_beyond_render_scale": None,
            "crops": [
                {
                    "feature": feature,
                    "baseline_sha256": None,
                    "msaa_sha256": None,
                    "correct": None,
                    "improved": None,
                }
                for feature in VISUAL_FEATURES
            ],
        },
    }


def _optional_uint(value, path, reasons, missing_reason, *, minimum=0):
    if value is None:
        reasons.append(missing_reason)
        return None
    return _uint(value, path, minimum=minimum)


def _optional_number(value, path, reasons, missing_reason, *, minimum=0):
    if value is None:
        reasons.append(missing_reason)
        return None
    return _finite_number(value, path, minimum=minimum)


def _optional_text(value, path, reasons, missing_reason):
    if value is None:
        reasons.append(missing_reason)
        return None
    return _text(value, path)


def _timings(value, path, reasons):
    if value is None:
        reasons.append(f"{path} evidence is missing")
        return None
    value = _object(value, path)
    _known_keys(value, PERCENTILES, path)
    result = {}
    for percentile in PERCENTILES:
        result[percentile] = _optional_number(
            value.get(percentile),
            f"{path}.{percentile}",
            reasons,
            f"{path}.{percentile} evidence is missing",
            minimum=0,
        )
        if result[percentile] == 0:
            reasons.append(f"{path}.{percentile} must be greater than zero")
    if all(result[percentile] is not None for percentile in PERCENTILES):
        if not (
            result["p50"] <= result["p95"] <= result["p99"]
        ):
            reasons.append(f"{path} percentiles are not monotonic")
    return result


def _decimal(value):
    return Decimal(str(value))


def _json_number(value):
    if value == value.to_integral_value():
        return int(value)
    return float(value)


def _evaluate_measurement(measurement, thresholds, costs, reasons):
    observed = {
        "gpu_time_regression_percent": {},
        "peak_attachment_bytes": None,
    }
    if measurement is None:
        reasons.append("measurement evidence is missing")
        return observed
    measurement = _object(measurement, "measurement")
    fields = (
        "control",
        "warmup_frames",
        "baseline_frame_count",
        "msaa_frame_count",
        "gpu_time_ms",
        "peak_attachment_bytes",
    )
    _known_keys(measurement, fields, "measurement")

    control = measurement.get("control")
    if control is None:
        reasons.append("measurement.control evidence is missing")
    else:
        control = _object(control, "measurement.control")
        _known_keys(control, CONTROL_FIELDS, "measurement.control")
        for field in CONTROL_FIELDS:
            _optional_text(
                control.get(field),
                f"measurement.control.{field}",
                reasons,
                f"measurement.control.{field} evidence is missing",
            )

    _optional_uint(
        measurement.get("warmup_frames"),
        "measurement.warmup_frames",
        reasons,
        "measurement.warmup_frames evidence is missing",
        minimum=1,
    )
    for field in ("baseline_frame_count", "msaa_frame_count"):
        count = _optional_uint(
            measurement.get(field),
            f"measurement.{field}",
            reasons,
            f"measurement.{field} evidence is missing",
            minimum=1,
        )
        if count is not None and count < thresholds["min_measured_frames"]:
            reasons.append(
                f"measurement.{field} {count} is below min_measured_frames "
                f"{thresholds['min_measured_frames']}"
            )

    gpu_time = measurement.get("gpu_time_ms")
    baseline = msaa = None
    if gpu_time is None:
        reasons.append("measurement.gpu_time_ms evidence is missing")
    else:
        gpu_time = _object(gpu_time, "measurement.gpu_time_ms")
        _known_keys(gpu_time, ("baseline", "msaa", "resolve_p95"), "measurement.gpu_time_ms")
        baseline = _timings(
            gpu_time.get("baseline"), "measurement.gpu_time_ms.baseline", reasons
        )
        msaa = _timings(gpu_time.get("msaa"), "measurement.gpu_time_ms.msaa", reasons)
        resolve = _optional_number(
            gpu_time.get("resolve_p95"),
            "measurement.gpu_time_ms.resolve_p95",
            reasons,
            "measurement.gpu_time_ms.resolve_p95 evidence is missing",
        )
        if (
            resolve is not None
            and _decimal(resolve) > _decimal(thresholds["max_resolve_gpu_time_ms"])
        ):
            reasons.append(
                "resolve GPU time "
                f"{resolve} ms exceeds max_resolve_gpu_time_ms "
                f"{thresholds['max_resolve_gpu_time_ms']} ms"
            )

    if baseline is not None and msaa is not None:
        for percentile in PERCENTILES:
            before = baseline[percentile]
            after = msaa[percentile]
            if before is None or after is None or before == 0:
                continue
            regression = ((_decimal(after) - _decimal(before)) * Decimal(100)
                          / _decimal(before))
            observed["gpu_time_regression_percent"][percentile] = _json_number(regression)
            if regression > _decimal(thresholds["max_gpu_time_regression_percent"]):
                reasons.append(
                    f"{percentile} GPU time regression "
                    f"{_json_number(regression)}% exceeds "
                    "max_gpu_time_regression_percent "
                    f"{thresholds['max_gpu_time_regression_percent']}%"
                )

    peak = _optional_uint(
        measurement.get("peak_attachment_bytes"),
        "measurement.peak_attachment_bytes",
        reasons,
        "measurement.peak_attachment_bytes evidence is missing",
    )
    observed["peak_attachment_bytes"] = peak
    if peak is not None:
        if peak < costs["retained_attachment_bytes"]:
            reasons.append(
                f"measured peak attachment bytes {peak} is below the computed lower bound "
                f"{costs['retained_attachment_bytes']}"
            )
        if peak > thresholds["max_attachment_bytes"]:
            reasons.append(
                f"measured peak attachment bytes {peak} exceeds max_attachment_bytes "
                f"{thresholds['max_attachment_bytes']}"
            )
    return observed


def _evaluate_correctness(correctness, reasons):
    if correctness is None:
        reasons.append("correctness evidence is missing")
        return
    correctness = _object(correctness, "correctness")
    _known_keys(correctness, CORRECTNESS_CHECKS, "correctness")
    for check in CORRECTNESS_CHECKS:
        value = correctness.get(check)
        if value is None:
            reasons.append(f"correctness.{check} evidence is missing")
        elif not isinstance(value, bool):
            raise EvidenceError(f"correctness.{check} must be a boolean")
        elif not value:
            reasons.append(f"correctness check failed: {check}")


def _visual_digest(value, path, reasons):
    if value is None:
        reasons.append(f"{path} evidence is missing")
        return None
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        raise EvidenceError(f"{path} must be a lowercase SHA-256 digest")
    return value


def _visual_boolean(value, path, reasons):
    if value is None:
        reasons.append(f"{path} evidence is missing")
        return None
    if not isinstance(value, bool):
        raise EvidenceError(f"{path} must be a boolean")
    return value


def _evaluate_visual(visual, reasons):
    if visual is None:
        reasons.append("visual evidence is missing")
        return
    visual = _object(visual, "visual")
    _known_keys(
        visual,
        ("reviewer", "visible_benefit_beyond_render_scale", "crops"),
        "visual",
    )
    _optional_text(
        visual.get("reviewer"),
        "visual.reviewer",
        reasons,
        "visual.reviewer evidence is missing",
    )
    benefit = _visual_boolean(
        visual.get("visible_benefit_beyond_render_scale"),
        "visual.visible_benefit_beyond_render_scale",
        reasons,
    )
    if benefit is False:
        reasons.append("visual review found no benefit beyond render scaling")

    crops = visual.get("crops")
    if crops is None:
        reasons.append("visual.crops evidence is missing")
        return
    if not isinstance(crops, list):
        raise EvidenceError("visual.crops must be an array")
    by_feature = {}
    fields = ("feature", "baseline_sha256", "msaa_sha256", "correct", "improved")
    for index, crop in enumerate(crops):
        path = f"visual.crops[{index}]"
        crop = _object(crop, path)
        _known_keys(crop, fields, path)
        feature = crop.get("feature")
        if not isinstance(feature, str) or feature not in VISUAL_FEATURES:
            raise EvidenceError(
                f"{path}.feature must be one of " + ", ".join(VISUAL_FEATURES)
            )
        if feature in by_feature:
            raise EvidenceError(f"visual.crops contains duplicate feature: {feature}")
        by_feature[feature] = crop

    improved_any = False
    for feature in VISUAL_FEATURES:
        crop = by_feature.get(feature)
        if crop is None:
            reasons.append(f"visual crop evidence is missing: {feature}")
            continue
        path = f"visual.crops[{feature}]"
        baseline = _visual_digest(crop.get("baseline_sha256"), path + ".baseline_sha256", reasons)
        msaa = _visual_digest(crop.get("msaa_sha256"), path + ".msaa_sha256", reasons)
        correct = _visual_boolean(crop.get("correct"), path + ".correct", reasons)
        improved = _visual_boolean(crop.get("improved"), path + ".improved", reasons)
        if correct is False:
            reasons.append(f"visual correctness failed: {feature}")
        if improved:
            improved_any = True
            if baseline is not None and baseline == msaa:
                reasons.append(
                    f"visual crop {feature} is marked improved but its digests are identical"
                )
    if not improved_any:
        reasons.append("no reference crop demonstrates a visual improvement")


def evaluate_record(record):
    record = _object(record, "record")
    fields = (
        "schema",
        "configuration",
        "estimated_costs",
        "thresholds",
        "measurement",
        "correctness",
        "visual",
    )
    _known_keys(record, fields, "record")
    _required(record, ("schema", "configuration", "thresholds"), "record")
    if record["schema"] != SCHEMA:
        raise EvidenceError(f"record.schema must be {SCHEMA}")

    configuration = validate_configuration(record["configuration"])
    thresholds = validate_thresholds(record["thresholds"])
    costs = compute_costs(configuration)
    recorded_costs = record.get("estimated_costs")
    if recorded_costs is not None:
        recorded_costs = _object(recorded_costs, "estimated_costs")
        _known_keys(recorded_costs, COST_FIELDS, "estimated_costs")
        _required(recorded_costs, COST_FIELDS, "estimated_costs")
        for field in COST_FIELDS:
            _uint(recorded_costs[field], f"estimated_costs.{field}")
        if recorded_costs != costs:
            raise EvidenceError("estimated_costs does not match the validated configuration")

    reasons = []
    if costs["retained_attachment_bytes"] > thresholds["max_attachment_bytes"]:
        reasons.append(
            f"computed retained attachment bytes {costs['retained_attachment_bytes']} "
            f"exceeds max_attachment_bytes {thresholds['max_attachment_bytes']}"
        )
    observed = _evaluate_measurement(
        record.get("measurement"), thresholds, costs, reasons
    )
    _evaluate_correctness(record.get("correctness"), reasons)
    _evaluate_visual(record.get("visual"), reasons)
    return {
        "schema": REPORT_SCHEMA,
        "record_schema": SCHEMA,
        "configuration": configuration,
        "costs": costs,
        "thresholds": thresholds,
        "observed": observed,
        "decision": {
            "verdict": "go" if not reasons else "no-go",
            "go": not reasons,
            "reasons": reasons,
        },
    }


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def loads_record(text):
    if not isinstance(text, str):
        raise EvidenceError("record input must be UTF-8 text")
    if len(text.encode("utf-8")) > MAX_RECORD_BYTES:
        raise EvidenceError(f"record exceeds {MAX_RECORD_BYTES} bytes")

    def reject_constant(value):
        raise EvidenceError(f"non-finite JSON number is not allowed: {value}")

    try:
        return json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=reject_constant,
        )
    except json.JSONDecodeError as error:
        raise EvidenceError(
            f"invalid JSON at line {error.lineno}, column {error.colno}: {error.msg}"
        ) from error


def load_record(path):
    if path == "-":
        return loads_record(sys.stdin.read(MAX_RECORD_BYTES + 1))
    source = Path(path)
    if not source.is_file():
        raise EvidenceError(f"record is not a regular file: {source}")
    if source.stat().st_size > MAX_RECORD_BYTES:
        raise EvidenceError(f"record exceeds {MAX_RECORD_BYTES} bytes")
    try:
        text = source.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise EvidenceError("record is not valid UTF-8") from error
    return loads_record(text)


def write_json(value, path):
    text = json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n"
    if path == "-":
        sys.stdout.write(text)
    else:
        Path(path).write_text(text, encoding="utf-8")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=(
            "Create or evaluate fail-closed evidence records for a future Metal "
            "MSAA decision. This tool does not enable MSAA."
        )
    )
    commands = parser.add_subparsers(dest="command", required=True)
    template = commands.add_parser("template", help="write an incomplete no-go record")
    template.add_argument("--width", type=int, required=True)
    template.add_argument("--height", type=int, required=True)
    template.add_argument("--sample-count", type=int, required=True)
    template.add_argument("--color-format", required=True)
    template.add_argument("--depth-stencil-format", required=True)
    template.add_argument("--render-scale", type=int, required=True)
    template.add_argument("--max-attachment-bytes", type=int, required=True)
    template.add_argument("--max-gpu-time-regression-percent", type=float, required=True)
    template.add_argument("--max-resolve-gpu-time-ms", type=float, required=True)
    template.add_argument("--min-measured-frames", type=int, required=True)
    template.add_argument("--output", default="-", help="output JSON path (default: stdout)")

    evaluate = commands.add_parser("evaluate", help="evaluate a measurement record")
    evaluate.add_argument("record", help="input JSON path, or - for stdin")
    evaluate.add_argument("--output", default="-", help="output JSON path (default: stdout)")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        if args.command == "template":
            value = make_template(
                {
                    "width": args.width,
                    "height": args.height,
                    "sample_count": args.sample_count,
                    "color_format": args.color_format,
                    "depth_stencil_format": args.depth_stencil_format,
                    "render_scale": args.render_scale,
                },
                {
                    "max_attachment_bytes": args.max_attachment_bytes,
                    "max_gpu_time_regression_percent":
                        args.max_gpu_time_regression_percent,
                    "max_resolve_gpu_time_ms": args.max_resolve_gpu_time_ms,
                    "min_measured_frames": args.min_measured_frames,
                },
            )
            write_json(value, args.output)
            return 0

        report = evaluate_record(load_record(args.record))
        write_json(report, args.output)
        return 0 if report["decision"]["go"] else 1
    except (EvidenceError, OSError) as error:
        print(f"metal-msaa-evidence: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
