import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.prepare_macos_input_hooks import HOOKS, prepare


class PrepareMacosInputHooksTest(unittest.TestCase):
    def setUp(self):
        self.source = b"".join(
            f"void sub_{address}(void)\n{{}}\n".encode("ascii")
            for address in HOOKS
        )
        self.digest = hashlib.sha256(self.source).hexdigest()

    def test_only_definitions_are_renamed(self):
        result = prepare(self.source, len(self.source), self.digest)
        for address in HOOKS:
            self.assertIn(f"void sub_{address}_gen(void)\n".encode("ascii"), result)
            self.assertNotIn(f"void sub_{address}(void)\n".encode("ascii"), result)

    def test_crlf_normalizes_before_fingerprint(self):
        result = prepare(self.source.replace(b"\n", b"\r\n"), len(self.source), self.digest)
        self.assertNotIn(b"\r", result)

    def test_windows_fingerprint_accepts_lf_source(self):
        windows_source = self.source.replace(b"\n", b"\r\n")
        result = prepare(self.source, len(windows_source),
                         hashlib.sha256(windows_source).hexdigest())
        self.assertNotIn(b"\r", result)

    def test_unverified_source_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "fingerprint"):
            prepare(self.source + b"extra", len(self.source), self.digest)

    def test_missing_or_duplicate_definition_is_rejected(self):
        for source in (self.source.replace(b"void sub_0016DC16(void)\n", b""),
                       self.source + b"void sub_0016DC16(void)\n{}\n"):
            with self.subTest(source=source):
                digest = hashlib.sha256(source).hexdigest()
                with self.assertRaisesRegex(ValueError, "exactly one"):
                    prepare(source, len(source), digest)

    def test_hardlinked_output_does_not_modify_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "recomp_0032.c"
            output = root / "derived.c"
            manifest = root / "fingerprints.json"
            source.write_bytes(self.source)
            os.link(source, output)
            manifest.write_text(json.dumps({"files": {"recomp_0032.c": {
                "size": len(self.source), "sha256": self.digest}}}), encoding="utf-8")
            subprocess.run([sys.executable, "-m", "tools.prepare_macos_input_hooks",
                            "--source", str(source), "--fingerprint", str(manifest),
                            "--output", str(output)], check=True)
            self.assertEqual(source.read_bytes(), self.source)
            self.assertIn(b"void sub_0016DC16_gen(void)", output.read_bytes())

    def test_bad_fingerprint_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "recomp_0032.c"
            output = root / "derived.c"
            manifest = root / "fingerprints.json"
            source.write_bytes(self.source)
            output.write_bytes(b"existing")
            manifest.write_text(json.dumps({"files": {"recomp_0032.c": {
                "size": len(self.source), "sha256": "0" * 64}}}), encoding="utf-8")
            result = subprocess.run([sys.executable, "-m", "tools.prepare_macos_input_hooks",
                                     "--source", str(source), "--fingerprint", str(manifest),
                                     "--output", str(output)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b"existing")


if __name__ == "__main__":
    unittest.main()
