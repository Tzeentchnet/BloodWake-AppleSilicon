import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_from_disc", ROOT / "tools" / "build_from_disc.py")
build_from_disc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_from_disc)


class BuildFromDiscTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.disc = self.root / "source"
        self.disc.mkdir()
        (self.disc / "default.xbe").write_bytes(b"synthetic test input")
        self.install = self.root / "install"
        self.args = ["--disc", str(self.disc), "--install-dir", str(self.install)]

        patches = {
            "check_python": mock.DEFAULT,
            "check_space": mock.DEFAULT,
            "get_toolkit": mock.DEFAULT,
            "check_disc": mock.DEFAULT,
            "run_pipeline": mock.DEFAULT,
            "verify_gen": mock.DEFAULT,
            "find_vs": mock.DEFAULT,
            "find_cmake": mock.DEFAULT,
            "build": mock.DEFAULT,
            "stage": mock.DEFAULT,
        }
        patcher = mock.patch.multiple(build_from_disc, **patches)
        self.mocks = patcher.start()
        self.addCleanup(patcher.stop)
        self.mocks["check_python"].return_value = []
        self.mocks["get_toolkit"].return_value = str(self.root / "toolkit")
        self.mocks["check_disc"].side_effect = self.prepare_disc
        self.mocks["run_pipeline"].return_value = 1.0
        self.mocks["verify_gen"].return_value = "verified-digest"
        self.mocks["find_vs"].return_value = ("visual-studio", "17.0")
        self.mocks["find_cmake"].return_value = ("cmake", "3.20")
        self.mocks["build"].return_value = ("bloodwake.exe", 1.0)

    def prepare_disc(self, args, kind, src, toolkit, game_dir, retail, progress):
        if not args.check_only:
            target = Path(game_dir)
            target.mkdir(parents=True)
            (target / "default.xbe").write_bytes((self.disc / "default.xbe").read_bytes())
        return {"label": "synthetic"}

    def run_main(self, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = build_from_disc.main(self.args + list(args))
        return result, output.getvalue()

    def test_prepare_only_verifies_without_building(self):
        result, output = self.run_main("--prepare-only")

        self.assertEqual(result, build_from_disc.EXIT_OK)
        self.assertIn("No executable was built", output)
        self.assertEqual((self.install / "work" / "xbe" / "default.xbe").read_bytes(),
                         b"synthetic test input")
        self.mocks["run_pipeline"].assert_called_once()
        self.mocks["verify_gen"].assert_called_once()
        for name in ("find_vs", "find_cmake", "build", "stage"):
            self.mocks[name].assert_not_called()

    def test_prepare_only_rejects_mismatched_generated_code(self):
        self.mocks["verify_gen"].side_effect = build_from_disc.Fail(
            build_from_disc.EXIT_GEN, "verify", "generated code mismatch")

        result, output = self.run_main("--prepare-only")

        self.assertEqual(result, build_from_disc.EXIT_GEN)
        self.assertIn("generated code mismatch", output)
        self.assertNotIn("Preparation complete", output)
        self.mocks["build"].assert_not_called()
        self.mocks["stage"].assert_not_called()

    def test_prepare_only_json_reports_verified_result(self):
        result, output = self.run_main("--prepare-only", "--progress", "json")

        self.assertEqual(result, build_from_disc.EXIT_OK)
        last = json.loads(output.splitlines()[-1])
        self.assertEqual(last["step"], "done")
        self.assertEqual(last["pct"], 100)
        self.assertEqual(last["generated_total_sha256"], "verified-digest")
        self.assertEqual(last["disc"]["label"], "synthetic")

    def test_prepare_only_stops_when_disc_fails(self):
        self.mocks["check_disc"].side_effect = build_from_disc.Fail(
            build_from_disc.EXIT_DISC, "fingerprint", "disc mismatch")

        result, output = self.run_main("--prepare-only")

        self.assertEqual(result, build_from_disc.EXIT_DISC)
        self.assertIn("disc mismatch", output)
        self.mocks["run_pipeline"].assert_not_called()
        self.mocks["verify_gen"].assert_not_called()

    def test_check_only_still_stops_before_preparation(self):
        result, _ = self.run_main("--check-only")

        self.assertEqual(result, build_from_disc.EXIT_OK)
        for name in ("get_toolkit", "run_pipeline", "verify_gen", "find_vs", "build", "stage"):
            self.mocks[name].assert_not_called()

    def test_full_build_still_requires_toolchain_and_stages(self):
        result, _ = self.run_main()

        self.assertEqual(result, build_from_disc.EXIT_OK)
        for name in ("find_vs", "find_cmake", "run_pipeline", "verify_gen", "build", "stage"):
            self.mocks[name].assert_called_once()

    def test_stop_options_are_mutually_exclusive(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exc:
            build_from_disc.main(self.args + ["--check-only", "--prepare-only"])
        self.assertEqual(exc.exception.code, 2)


class GeneratedFingerprintTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.gen = self.root / "gen"
        self.gen.mkdir()
        self.source = self.gen / "fixture.c"
        self.source.write_bytes(b"first line\nsecond line\n")
        reference = b"first line\r\nsecond line\r\n"
        digest = hashlib.sha256(reference).hexdigest()
        self.total = build_from_disc.total_digest([("fixture.c", digest)])
        (self.root / "generated.json").write_text(json.dumps({
            "total_sha256": self.total,
            "files": {"fixture.c": {"size": len(reference), "sha256": digest}},
            "inputs": {},
        }), encoding="utf-8")
        patcher = mock.patch.object(build_from_disc, "FINGERPRINTS", str(self.root))
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_lf_output_matches_windows_fingerprint_without_rewriting(self):
        progress = mock.Mock()
        with mock.patch.object(build_from_disc.os, "linesep", "\n"):
            result = build_from_disc.verify_gen(str(ROOT), str(self.gen), progress)
        self.assertEqual(result, self.total)
        self.assertEqual(self.source.read_bytes(), b"first line\nsecond line\n")
        self.assertIn("line endings normalized", progress.call_args.args[2])

    def test_non_newline_change_is_rejected(self):
        self.source.write_bytes(b"first line\nchanged line\n")
        with mock.patch.object(build_from_disc.os, "linesep", "\n"):
            with self.assertRaises(build_from_disc.Fail) as exc:
                build_from_disc.verify_gen(str(ROOT), str(self.gen), mock.Mock())
        self.assertEqual(exc.exception.code, build_from_disc.EXIT_GEN)
        self.assertIn("fixture.c", exc.exception.msg)

    def test_extra_file_is_rejected(self):
        (self.gen / "extra.c").write_bytes(b"extra\n")
        with mock.patch.object(build_from_disc.os, "linesep", "\n"):
            with self.assertRaises(build_from_disc.Fail) as exc:
                build_from_disc.verify_gen(str(ROOT), str(self.gen), mock.Mock())
        self.assertEqual(exc.exception.code, build_from_disc.EXIT_GEN)
        self.assertIn("extra.c", exc.exception.msg)
        self.assertNotIn("fixture.c", exc.exception.msg)

    def test_missing_file_is_rejected(self):
        self.source.unlink()
        with mock.patch.object(build_from_disc.os, "linesep", "\n"):
            with self.assertRaises(build_from_disc.Fail) as exc:
                build_from_disc.verify_gen(str(ROOT), str(self.gen), mock.Mock())
        self.assertEqual(exc.exception.code, build_from_disc.EXIT_GEN)
        self.assertIn("fixture.c", exc.exception.msg)

    def test_windows_still_requires_exact_bytes(self):
        with mock.patch.object(build_from_disc.os, "linesep", "\r\n"):
            with self.assertRaises(build_from_disc.Fail) as exc:
                build_from_disc.verify_gen(str(ROOT), str(self.gen), mock.Mock())
        self.assertEqual(exc.exception.code, build_from_disc.EXIT_GEN)

    def test_exact_fingerprint_still_passes(self):
        self.source.write_bytes(b"first line\r\nsecond line\r\n")
        progress = mock.Mock()
        self.assertEqual(build_from_disc.verify_gen(str(ROOT), str(self.gen), progress), self.total)
        self.assertNotIn("line endings normalized", progress.call_args.args[2])


if __name__ == "__main__":
    unittest.main()
