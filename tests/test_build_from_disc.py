import contextlib
import hashlib
import importlib.util
import io
import json
import os
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
            "build_host": mock.DEFAULT,
            "find_vs": mock.DEFAULT,
            "find_apple_clang": mock.DEFAULT,
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
        self.mocks["build_host"].return_value = "windows"
        self.mocks["find_vs"].return_value = ("visual-studio", "17.0")
        self.mocks["find_apple_clang"].return_value = "/toolchain/clang"
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
        for name in ("build_host", "find_vs", "find_apple_clang", "find_cmake", "build", "stage"):
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

    def test_macos_work_on_another_volume_gets_its_own_space_check(self):
        work = self.root / "external-work"
        with mock.patch.object(build_from_disc.sys, "platform", "darwin"), \
             mock.patch.object(build_from_disc, "same_storage_volume", return_value=False):
            result, _ = self.run_main("--prepare-only", "--work-dir", str(work))

        self.assertEqual(result, build_from_disc.EXIT_OK)
        self.assertEqual(self.mocks["check_space"].call_args_list, [
            mock.call(str(self.install), build_from_disc.NEED_INSTALL,
                      "identify", "the game's files"),
            mock.call(str(work), build_from_disc.NEED_WORK, "identify", "the build"),
        ])

    def test_macos_work_on_same_volume_combines_space_check(self):
        with mock.patch.object(build_from_disc.sys, "platform", "darwin"), \
             mock.patch.object(build_from_disc, "same_storage_volume", return_value=True):
            result, _ = self.run_main("--prepare-only")

        self.assertEqual(result, build_from_disc.EXIT_OK)
        self.mocks["check_space"].assert_called_once_with(
            str(self.install), build_from_disc.NEED_INSTALL + build_from_disc.NEED_WORK,
            "identify", "the game's files and the build")

    def test_check_only_still_stops_before_preparation(self):
        result, _ = self.run_main("--check-only")

        self.assertEqual(result, build_from_disc.EXIT_OK)
        for name in ("get_toolkit", "run_pipeline", "verify_gen", "build_host", "find_vs",
                     "find_apple_clang", "build", "stage"):
            self.mocks[name].assert_not_called()

    def test_full_build_still_requires_toolchain_and_stages(self):
        result, output = self.run_main()

        self.assertEqual(result, build_from_disc.EXIT_OK)
        for name in ("find_vs", "find_cmake", "run_pipeline", "verify_gen", "build", "stage"):
            self.mocks[name].assert_called_once()
        self.mocks["find_apple_clang"].assert_not_called()
        self.assertEqual(self.mocks["build"].call_args.kwargs["host"], "windows")
        self.assertEqual(self.mocks["stage"].call_args.kwargs["host"], "windows")
        record = self.mocks["stage"].call_args.args[3]
        self.assertEqual(record["editions"], ["original", "remastered"])
        self.assertEqual(record["tools"]["visual_studio"], "17.0")
        self.assertEqual(record["paths"]["exe"], str(self.install / "build" / "Release" / "bloodwake.exe"))
        self.assertIn("Blood Wake.cmd", output)

    def test_macos_full_build_uses_native_toolchain_and_install(self):
        self.mocks["build_host"].return_value = "macos"
        self.mocks["build"].return_value = ("bloodwake", 1.0)

        result, output = self.run_main()

        self.assertEqual(result, build_from_disc.EXIT_OK)
        self.mocks["find_vs"].assert_not_called()
        self.mocks["find_apple_clang"].assert_called_once()
        self.mocks["find_cmake"].assert_called_once_with(None)
        self.assertEqual(self.mocks["build"].call_args.kwargs["host"], "macos")
        self.assertEqual(self.mocks["stage"].call_args.kwargs["host"], "macos")
        record = self.mocks["stage"].call_args.args[3]
        self.assertEqual(record["editions"], ["original"])
        self.assertEqual(record["tools"]["apple_clang"], "/toolchain/clang")
        self.assertNotIn("visual_studio", record["tools"])
        self.assertEqual(record["paths"]["exe"], str(self.install / "build" / "Release" / "bloodwake"))
        self.assertIn("./build/Release/bloodwake", output)
        self.assertNotIn("Blood Wake.cmd", output)

    def test_macos_missing_clang_fails_before_unpacking(self):
        self.mocks["build_host"].return_value = "macos"
        self.mocks["find_apple_clang"].return_value = None

        result, output = self.run_main()

        self.assertEqual(result, build_from_disc.EXIT_DEP)
        self.assertIn("Xcode Command Line Tools", output)
        self.mocks["find_vs"].assert_not_called()
        self.mocks["find_cmake"].assert_not_called()
        self.mocks["check_disc"].assert_not_called()

    def test_macos_generated_mismatch_never_builds_or_stages(self):
        self.mocks["build_host"].return_value = "macos"
        self.mocks["verify_gen"].side_effect = build_from_disc.Fail(
            build_from_disc.EXIT_GEN, "verify", "generated code mismatch")

        result, output = self.run_main()

        self.assertEqual(result, build_from_disc.EXIT_GEN)
        self.assertIn("generated code mismatch", output)
        self.mocks["build"].assert_not_called()
        self.mocks["stage"].assert_not_called()

    def test_stop_options_are_mutually_exclusive(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exc:
            build_from_disc.main(self.args + ["--check-only", "--prepare-only"])
        self.assertEqual(exc.exception.code, 2)


class BuildHostTests(unittest.TestCase):
    def test_windows_and_native_macos_are_supported(self):
        with mock.patch.object(build_from_disc.sys, "platform", "win32"):
            self.assertEqual(build_from_disc.build_host(), "windows")
        with mock.patch.object(build_from_disc.sys, "platform", "darwin"), \
             mock.patch.object(build_from_disc.platform, "machine", return_value="arm64"):
            self.assertEqual(build_from_disc.build_host(), "macos")

    def test_macos_rosetta_or_intel_and_other_hosts_fail_clearly(self):
        with mock.patch.object(build_from_disc.sys, "platform", "darwin"), \
             mock.patch.object(build_from_disc.platform, "machine", return_value="x86_64"):
            with self.assertRaises(build_from_disc.Fail) as exc:
                build_from_disc.build_host()
            self.assertEqual(exc.exception.code, build_from_disc.EXIT_DEP)
            self.assertIn("Apple Silicon", exc.exception.msg)
        with mock.patch.object(build_from_disc.sys, "platform", "linux"):
            with self.assertRaises(build_from_disc.Fail) as exc:
                build_from_disc.build_host()
            self.assertIn("--prepare-only", exc.exception.msg)


class StorageVolumeTests(unittest.TestCase):
    def test_macos_nonexistent_paths_on_same_volume(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(build_from_disc.sys, "platform", "darwin"):
            first = Path(directory) / "new" / "install"
            second = Path(directory) / "other" / "work"
            self.assertEqual(build_from_disc.existing_ancestor(str(first)), directory)
            self.assertTrue(build_from_disc.same_storage_volume(str(first), str(second)))

    def test_macos_uses_devices_of_nearest_existing_ancestors(self):
        with mock.patch.object(build_from_disc.sys, "platform", "darwin"), \
             mock.patch.object(build_from_disc, "existing_ancestor",
                               side_effect=["/one", "/two"]), \
             mock.patch.object(build_from_disc.os, "stat", side_effect=[
                 mock.Mock(st_dev=1), mock.Mock(st_dev=2)]) as stat:
            self.assertFalse(build_from_disc.same_storage_volume("/one/new", "/two/new"))
        self.assertEqual(stat.call_args_list, [mock.call("/one"), mock.call("/two")])

    def test_macos_volume_stat_failure_is_reported(self):
        with mock.patch.object(build_from_disc.sys, "platform", "darwin"), \
             mock.patch.object(build_from_disc, "existing_ancestor", return_value="/one"), \
             mock.patch.object(build_from_disc.os, "stat", side_effect=OSError("unavailable")):
            with self.assertRaises(build_from_disc.Fail) as exc:
                build_from_disc.same_storage_volume("/one/new", "/two/new")
        self.assertEqual(exc.exception.code, build_from_disc.EXIT_IO)
        self.assertIn("Can't compare install and work volumes", exc.exception.msg)

    def test_windows_keeps_drive_based_comparison(self):
        with mock.patch.object(build_from_disc.sys, "platform", "win32"), \
             mock.patch.object(build_from_disc.os.path, "splitdrive",
                               side_effect=[("C:", "\\install"), ("D:", "\\work")]), \
             mock.patch.object(build_from_disc.os, "stat") as stat:
            self.assertFalse(build_from_disc.same_storage_volume("C:\\install", "D:\\work"))
        stat.assert_not_called()


class BuildCommandTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "source tree"
        self.toolkit = self.root / "toolkit"
        self.gen = self.root / "generated"
        self.work = self.root / "work dir"
        self.repo.mkdir()
        self.toolkit.mkdir()
        self.gen.mkdir()
        self.work.mkdir()

    def run_build(self, host, produce_exe=True):
        commands = []
        progress = mock.Mock()

        def run_logged(cmd, log, **kwargs):
            commands.append(cmd)
            if "--build" in cmd:
                if host == "macos":
                    for _ in range(10):
                        kwargs["on_line"]("[ 50%] Building C object CMakeFiles/bloodwake.dir/source.c.o\n")
                    exe = self.work / "build" / "bloodwake"
                else:
                    exe = self.work / "build" / "Release" / "bloodwake.exe"
                if produce_exe:
                    exe.parent.mkdir(parents=True, exist_ok=True)
                    exe.write_bytes(b"synthetic executable")
            return 0

        with mock.patch.object(build_from_disc, "run_logged", side_effect=run_logged):
            result = build_from_disc.build(str(self.repo), str(self.toolkit), str(self.gen),
                                           str(self.work), "cmake", 3, progress, host=host)
        return result, commands, progress

    def test_macos_configures_arm64_release_and_builds_only_native_target(self):
        (exe, _), commands, progress = self.run_build("macos")
        cfg, build = commands
        self.assertEqual(exe, str(self.work / "build" / "bloodwake"))
        self.assertIn("Unix Makefiles", cfg)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", cfg)
        self.assertIn("-DCMAKE_OSX_ARCHITECTURES=arm64", cfg)
        self.assertIn("-DXBOXRECOMP_DIR=" + str(self.toolkit).replace("\\", "/"), cfg)
        self.assertIn("-DBW_GEN_DIR=" + str(self.gen).replace("\\", "/"), cfg)
        self.assertNotIn("-A", cfg)
        self.assertEqual(build[-2:], ["--target", "bloodwake"])
        self.assertTrue(any("compiling (10 " in call.args[2] for call in progress.call_args_list))

    def test_windows_configuration_and_executable_layout_are_preserved(self):
        (exe, _), commands, _ = self.run_build("windows")
        cfg, build = commands
        self.assertEqual(exe, str(self.work / "build" / "Release" / "bloodwake.exe"))
        self.assertIn("Visual Studio 17 2022", cfg)
        self.assertEqual(cfg[cfg.index("-A") + 1], "x64")
        self.assertNotIn("--target", build)

    def test_missing_native_executable_fails(self):
        with self.assertRaises(build_from_disc.Fail) as exc:
            self.run_build("macos", produce_exe=False)
        self.assertEqual(exc.exception.code, build_from_disc.EXIT_BUILD)
        self.assertIn("bloodwake", exc.exception.msg)

    def test_macos_stage_copies_only_native_executable_and_record(self):
        exe = self.root / "bloodwake"
        exe.write_bytes(b"synthetic executable")
        exe.chmod(0o755)
        inst = self.root / "install dir"
        disc = inst / "game" / "disc"
        disc.mkdir(parents=True)
        (disc / "sentinel").write_bytes(b"synthetic disc data")
        build_from_disc.stage(str(self.repo), str(exe), str(inst), {"format": 1}, mock.Mock(), host="macos")
        staged = inst / "build" / "Release" / "bloodwake"
        self.assertEqual(staged.read_bytes(), exe.read_bytes())
        if os.name != "nt":
            self.assertTrue(staged.stat().st_mode & 0o111)
        self.assertEqual(json.loads((inst / "install.json").read_text()), {"format": 1})
        self.assertFalse((inst / "Blood Wake.cmd").exists())
        self.assertFalse((inst / "tools").exists())
        self.assertEqual((disc / "sentinel").read_bytes(), b"synthetic disc data")

    def test_windows_stage_keeps_launcher_and_pdb(self):
        tools = self.repo / "tools"
        tools.mkdir()
        for name in ("launcher.pyw", "play.py"):
            (tools / name).write_text("synthetic launcher")
        exe = self.root / "bloodwake.exe"
        exe.write_bytes(b"synthetic executable")
        (self.root / "bloodwake.pdb").write_bytes(b"synthetic symbols")
        inst = self.root / "install"
        build_from_disc.stage(str(self.repo), str(exe), str(inst), {"format": 1}, mock.Mock())
        self.assertTrue((inst / "build" / "Release" / "bloodwake.exe").is_file())
        self.assertTrue((inst / "build" / "Release" / "bloodwake.pdb").is_file())
        self.assertTrue((inst / "tools" / "launcher.pyw").is_file())
        self.assertTrue((inst / "tools" / "play.py").is_file())
        self.assertIn(b"@echo off\r\n", (inst / "Blood Wake.cmd").read_bytes())
        self.assertTrue((inst / "play.cmd").is_file())


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
