#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "embed_metal_shader.py"
SPEC = importlib.util.spec_from_file_location("embed_metal_shader", MODULE_PATH)
assert SPEC and SPEC.loader
EMBED = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EMBED)


class EmbedMetalShaderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.source = ROOT / "src" / "gpu" / "bw_metal_shaders.metal"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_source_only_is_stable_and_bounded(self) -> None:
        first = self.directory / "first.inc"
        second = self.directory / "second.inc"
        EMBED.generate(self.source, first)
        EMBED.generate(self.source, second)
        text = first.read_text(encoding="ascii")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertIn("#define BW_METAL_OFFLINE_SHADER_LIBRARIES 0", text)
        self.assertIn(
            's_metal_shader_source_sha256[] = '
            '"3fda260b7e0584eeea7d6a6d55229854dd67b310a447f9c1b45e21f617a2dec7"',
            text,
        )
        self.assertIn("s_metal_shader_source_size = 20571u", text)
        self.assertIn("s_metal_shader_library_default_size = 0u", text)
        self.assertNotIn(str(ROOT), text)

    def test_paired_libraries_are_embedded(self) -> None:
        default = self.directory / "default.metallib"
        precise = self.directory / "precise.metallib"
        output = self.directory / "shader.inc"
        default.write_bytes(b"MTLB-default")
        precise.write_bytes(b"MTLB-precise")
        EMBED.generate(self.source, output, default, precise)
        text = output.read_text(encoding="ascii")
        self.assertIn("#define BW_METAL_OFFLINE_SHADER_LIBRARIES 1", text)
        self.assertIn("s_metal_shader_library_default_size = 12u", text)
        self.assertIn("s_metal_shader_library_precise_size = 12u", text)

    def test_incomplete_or_symlinked_inputs_are_rejected(self) -> None:
        library = self.directory / "default.metallib"
        alias = self.directory / "alias.metallib"
        output = self.directory / "shader.inc"
        library.write_bytes(b"MTLB")
        alias.symlink_to(library)
        with self.assertRaisesRegex(ValueError, "supplied together"):
            EMBED.generate(self.source, output, library, None)
        with self.assertRaisesRegex(ValueError, "must not be a symlink"):
            EMBED.generate(self.source, output, alias, library)

    def test_missing_entry_points_and_limits_are_rejected(self) -> None:
        invalid_source = self.directory / "invalid.metal"
        output = self.directory / "shader.inc"
        invalid_source.write_text("#include <metal_stdlib>\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "required entry point"):
            EMBED.generate(invalid_source, output)
        excessive = self.directory / "large.metallib"
        with excessive.open("wb") as stream:
            stream.truncate(EMBED.MAX_LIBRARY_BYTES + 1)
        valid = self.directory / "valid.metallib"
        valid.write_bytes(b"MTLB")
        with self.assertRaisesRegex(ValueError, "exceeds"):
            EMBED.generate(self.source, output, excessive, valid)


if __name__ == "__main__":
    unittest.main()
