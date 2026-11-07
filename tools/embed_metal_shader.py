#!/usr/bin/env python3
"""Generate bounded C data for the stable Metal source and offline libraries."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import stat


MAX_SOURCE_BYTES = 1024 * 1024
MAX_LIBRARY_BYTES = 16 * 1024 * 1024
REQUIRED_ENTRY_POINTS = (
    "vertex_main",
    "color_main",
    "texture_main",
    "extended_vertex",
    "extended_fragment",
    "extended_specialized_fragment",
    "realistic_water_fragment",
    "clear_vertex",
    "clear_color",
    "clear_depth",
    "present_vertex",
    "present_resolve",
    "present_fragment",
    "scale_fragment",
    "box_fragment",
)


def _read_regular(path: Path, limit: int, kind: str) -> bytes:
    if path.is_symlink():
        raise ValueError(f"{kind} must not be a symlink: {path}")
    info = path.stat()
    if not stat.S_ISREG(info.st_mode):
        raise ValueError(f"{kind} must be a regular file: {path}")
    if info.st_size <= 0:
        raise ValueError(f"{kind} is empty: {path}")
    if info.st_size > limit:
        raise ValueError(f"{kind} exceeds {limit} bytes: {path}")
    data = path.read_bytes()
    if len(data) != info.st_size:
        raise ValueError(f"{kind} changed while being read: {path}")
    return data


def _read_source(path: Path) -> bytes:
    source = _read_regular(path, MAX_SOURCE_BYTES, "Metal source")
    if b"\0" in source:
        raise ValueError("Metal source contains a NUL byte")
    text = source.decode("utf-8")
    missing = [
        name for name in REQUIRED_ENTRY_POINTS
        if text.count(f"{name}(") != 1
    ]
    if missing:
        raise ValueError(
            "Metal source must define each required entry point exactly once: "
            + ", ".join(missing)
        )
    return source


def _definition(name: str, data: bytes) -> str:
    storage = data or b"\0"
    rows = []
    for offset in range(0, len(storage), 12):
        values = ", ".join(f"0x{value:02x}" for value in storage[offset:offset + 12])
        rows.append(f"    {values},")
    return (
        f"static const uint8_t {name}[] = {{\n"
        + "\n".join(rows)
        + f"\n}};\nstatic const size_t {name}_size = {len(data)}u;\n"
    )


def generate(
    source_path: Path,
    output_path: Path,
    default_library_path: Path | None = None,
    precise_library_path: Path | None = None,
) -> None:
    if (default_library_path is None) != (precise_library_path is None):
        raise ValueError("default and precise Metal libraries must be supplied together")
    source = _read_source(source_path)
    default_library = (
        _read_regular(default_library_path, MAX_LIBRARY_BYTES, "default Metal library")
        if default_library_path is not None else b""
    )
    precise_library = (
        _read_regular(precise_library_path, MAX_LIBRARY_BYTES, "precise Metal library")
        if precise_library_path is not None else b""
    )
    source_hash = hashlib.sha256(source).hexdigest()
    content = (
        "/* Generated from project-owned Metal source. Do not edit. */\n"
        f"#define BW_METAL_OFFLINE_SHADER_LIBRARIES {int(bool(default_library))}\n"
        f"static const char s_metal_shader_source_sha256[] = \"{source_hash}\";\n"
        + _definition("s_metal_shader_source", source)
        + _definition("s_metal_shader_library_default", default_library)
        + _definition("s_metal_shader_library_precise", precise_library)
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(f".{output_path.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("w", encoding="ascii", newline="\n") as stream:
            stream.write(content)
        os.replace(temporary, output_path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--default-library", type=Path)
    parser.add_argument("--precise-library", type=Path)
    args = parser.parse_args()
    try:
        generate(
            args.source,
            args.output,
            args.default_library,
            args.precise_library,
        )
    except (OSError, UnicodeError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
