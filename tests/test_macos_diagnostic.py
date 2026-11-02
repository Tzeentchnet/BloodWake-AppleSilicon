import contextlib
import importlib.util
import io
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("macos_diagnostic", ROOT / "tools/macos_diagnostic.py")
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class MacosDiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.prepared = self.root / "prepared"
        self.toolkit = self.root / "toolkit"
        self.repository = self.root / "repository"
        for directory in (self.prepared, self.toolkit, self.repository):
            directory.mkdir()

    def test_snapshot_detects_content_modes_and_directories(self):
        file = self.prepared / "file"
        file.write_bytes(b"first")
        before = diagnostic.snapshot_tree(self.prepared)
        self.assertIn(".", before)
        file.write_bytes(b"other")
        self.assertNotEqual(before, diagnostic.snapshot_tree(self.prepared))
        file.write_bytes(b"first")
        file.chmod(before["file"][1] ^ stat.S_IWUSR)
        self.assertNotEqual(before, diagnostic.snapshot_tree(self.prepared))
        file.chmod(before["file"][1])
        (self.prepared / "new-directory").mkdir()
        self.assertNotEqual(before, diagnostic.snapshot_tree(self.prepared))

    @unittest.skipIf(os.name == "nt", "symlink creation may require Windows privileges")
    def test_snapshot_rejects_external_and_dangling_symlinks(self):
        inside = self.prepared / "file"
        inside.write_bytes(b"inside")
        link = self.prepared / "link"
        link.symlink_to("file")
        self.assertEqual(diagnostic.snapshot_tree(self.prepared)["link"][0], "symlink")
        link.unlink()
        external = self.root / "external"
        external.write_bytes(b"not in the prepared tree")
        link.symlink_to(external)
        with self.assertRaisesRegex(RuntimeError, "symlink outside"):
            diagnostic.snapshot_tree(self.prepared)
        link.unlink()
        link.symlink_to("missing")
        with self.assertRaises(FileNotFoundError):
            diagnostic.snapshot_tree(self.prepared)

    def test_snapshot_reports_directory_traversal_errors(self):
        def inaccessible(_root, *, followlinks, onerror):
            self.assertFalse(followlinks)
            onerror(PermissionError("directory inaccessible"))

        with mock.patch.object(diagnostic.os, "walk", side_effect=inaccessible):
            with self.assertRaisesRegex(PermissionError, "directory inaccessible"):
                diagnostic.snapshot_tree(self.prepared)

    def test_new_private_output_must_not_overlap_protected_trees(self):
        with mock.patch.object(diagnostic, "ROOT", self.repository):
            output = diagnostic.create_output(self.root / "output", self.prepared, self.toolkit)
            if os.name != "nt":
                self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o700)
            with self.assertRaises(FileExistsError):
                diagnostic.create_output(output, self.prepared, self.toolkit)
            for protected in (self.prepared, self.toolkit, self.repository):
                with self.subTest(protected=protected), self.assertRaises(ValueError):
                    diagnostic.create_output(protected / "output", self.prepared, self.toolkit)
            with self.assertRaises(ValueError):
                diagnostic.create_output(self.root, self.prepared, self.toolkit)

    def test_profile_escapes_paths_and_rejects_control_characters(self):
        profile = diagnostic.sandbox_profile(Path('/tmp/a "quoted" path'))
        self.assertIn(r'\"quoted\"', profile)
        self.assertIn("(deny network*)", profile)
        self.assertIn("(deny process-fork)", profile)
        self.assertIn("(deny file-write*)", profile)
        with self.assertRaises(ValueError):
            diagnostic.sandbox_profile(Path("/tmp/bad\npath"))

    def test_guest_environment_is_allowlisted_and_opt_in(self):
        with mock.patch.dict(os.environ, {"RECOMP_USB": "1", "RECOMP_AC97_READY": "0",
                                         "BW_MACOS_AUDIO": "0",
                                         "BW_EDITION": "remastered",
                                         "BW_SETTINGS": "unsafe",
                                         "BW_MACOS_KEYBOARD_HOOK": "1",
                                         "BW_SYNTHETIC_INPUT_DIAGNOSTIC": "1"}, clear=True):
            env = diagnostic.diagnostic_env(self.root, self.prepared, 15, False, False, 4, 5)
        self.assertNotIn("RECOMP_USB", env)
        self.assertNotIn("RECOMP_AC97_READY", env)
        self.assertNotIn("BW_MACOS_AUDIO", env)
        self.assertNotIn("BW_METAL_QUINCUNX_APPROX", env)
        self.assertNotIn("BW_GPU_AUDIT", env)
        self.assertNotIn("BW_MACOS_KEYBOARD_HOOK", env)
        self.assertNotIn("BW_SYNTHETIC_INPUT_DIAGNOSTIC", env)
        self.assertEqual(env["BW_SETTINGS"], "none")
        self.assertEqual(env["BW_EDITION"], "original")
        self.assertEqual(env["BW_RUN_SECONDS"], "15")
        self.assertEqual(env["BW_NATIVE_LAUNCH_IN_FD"], "4")
        enabled = diagnostic.diagnostic_env(self.root, self.prepared, 15, True, True, 4, 5,
                                            keyboard_hook=True)
        self.assertEqual(enabled["BW_METAL_QUINCUNX_APPROX"], "1")
        self.assertEqual(enabled["BW_GPU_AUDIT"], "1")
        self.assertEqual(enabled["BW_GPU_AUDIT_FILE"], str(self.root / "gpu-audit.txt"))
        self.assertEqual(enabled["BW_MACOS_KEYBOARD_HOOK"], "1")
        self.assertNotIn("BW_SYNTHETIC_INPUT_DIAGNOSTIC", enabled)
        synthetic = diagnostic.diagnostic_env(self.root, self.prepared, 15, False, False, 4, 5,
                                               keyboard_hook=True, synthetic_input=True)
        self.assertEqual(synthetic["BW_SYNTHETIC_INPUT_DIAGNOSTIC"], "1")
        remastered = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5, edition="remastered")
        self.assertEqual(remastered["BW_EDITION"], "remastered")
        audio_disabled = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5,
            disable_audio=True)
        self.assertEqual(audio_disabled["BW_MACOS_AUDIO"], "0")
        with self.assertRaisesRegex(ValueError, "edition must be original or remastered"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5, edition="Remastered")
        with self.assertRaisesRegex(ValueError, "requires the experimental keyboard hook"):
            diagnostic.diagnostic_env(self.root, self.prepared, 15, False, False, 4, 5,
                                      synthetic_input=True)

    def test_sandbox_preflight_fails_closed(self):
        failure = subprocess.CompletedProcess([], 1, "", "sandbox denied setup")
        output = self.root / "output"
        output.mkdir()
        with mock.patch.object(diagnostic.subprocess, "run", return_value=failure):
            with self.assertRaisesRegex(RuntimeError, "sandbox preflight failed"):
                diagnostic.verify_sandbox("profile", output, self.prepared / "file")

    @unittest.skipUnless(sys.platform == "darwin", "macOS sandbox required")
    def test_real_sandbox_denies_network_writes_and_forks(self):
        protected = self.prepared / "file"
        protected.write_bytes(b"unchanged")
        output = self.root / "output"
        output.mkdir()
        diagnostic.verify_sandbox(diagnostic.sandbox_profile(output), output, protected)
        self.assertEqual(protected.read_bytes(), b"unchanged")

    @unittest.skipIf(os.name == "nt", "Windows selectors do not support pipes")
    def test_guest_uses_private_pipes_and_preserves_relaunch_request(self):
        child = mock.Mock()
        child.wait.return_value = 0
        child.poll.return_value = 0

        def spawn(command, **kwargs):
            self.assertEqual(command[-1], "--bw-native-child")
            self.assertEqual(kwargs["cwd"], self.root)
            self.assertEqual(kwargs["stdout"], subprocess.PIPE)
            env = kwargs["env"]
            input_fd = int(env["BW_NATIVE_LAUNCH_IN_FD"])
            output_fd = int(env["BW_NATIVE_LAUNCH_OUT_FD"])
            self.assertEqual(kwargs["pass_fds"], (input_fd, output_fd))
            self.assertEqual(os.read(input_fd, 1), b"")
            os.write(output_fd, b"private relaunch")
            read_fd, write_fd = os.pipe()
            os.write(write_fd, b"guest output")
            os.close(write_fd)
            child.stdout = os.fdopen(read_fd, "rb")
            return child

        with mock.patch.object(diagnostic.subprocess, "Popen", side_effect=spawn):
            status = diagnostic.run_guest(Path("/native"), self.root, self.prepared,
                                          "profile", 20, False, False)
        self.assertEqual(status, 0)
        child.wait.assert_called_once()
        self.assertGreater(child.wait.call_args.kwargs["timeout"], 0)
        self.assertEqual((self.root / "relaunch.bin").read_bytes(), b"private relaunch")
        self.assertEqual((self.root / "guest.log").read_bytes(), b"guest output")

    @unittest.skipIf(os.name == "nt", "Windows selectors do not support pipes")
    def test_external_timeout_kills_guest_and_reports_failure(self):
        child = mock.Mock()
        child.poll.return_value = None
        read_fd, write_fd = os.pipe()
        output_read, output_write = os.pipe()
        self.addCleanup(os.close, write_fd)
        self.addCleanup(os.close, output_read)
        self.addCleanup(os.close, output_write)
        child.stdout = os.fdopen(read_fd, "rb")
        with mock.patch.object(diagnostic.time, "monotonic", side_effect=[100, 102]):
            with self.assertRaisesRegex(RuntimeError, "external deadline"):
                diagnostic.capture_guest_output(child, output_read, io.BytesIO(), 1)
        child.kill.assert_called_once_with()
        child.wait.assert_called_once_with()

    @unittest.skipIf(os.name == "nt", "Windows selectors do not support pipes")
    def test_guest_log_limit_is_bounded_and_fails_explicitly(self):
        child = mock.Mock()
        child.poll.return_value = None
        read_fd, write_fd = os.pipe()
        output_read, output_write = os.pipe()
        os.write(write_fd, b"123456")
        os.close(write_fd)
        os.close(output_write)
        self.addCleanup(os.close, output_read)
        child.stdout = os.fdopen(read_fd, "rb")
        log = io.BytesIO()
        with mock.patch.object(diagnostic, "MAX_LOG_BYTES", 5):
            with self.assertRaisesRegex(RuntimeError, "log exceeded"):
                diagnostic.capture_guest_output(child, output_read, log, 5)
        self.assertEqual(log.getvalue(), b"12345")
        child.kill.assert_called_once_with()

    def test_pipe_creation_failure_closes_earlier_descriptors(self):
        descriptors = os.pipe()
        with mock.patch.object(diagnostic.os, "pipe", side_effect=[descriptors, OSError("pipe failed")]):
            with self.assertRaisesRegex(OSError, "pipe failed"):
                diagnostic.run_guest(Path("/native"), self.root, self.prepared,
                                     "profile", 1, False, False)
        for descriptor in descriptors:
            with self.assertRaises(OSError):
                os.fstat(descriptor)

    def test_guest_is_reaped_if_capture_setup_fails(self):
        child = mock.Mock()
        child.poll.return_value = None
        with mock.patch.object(diagnostic.subprocess, "Popen", return_value=child), \
             mock.patch.object(diagnostic, "capture_guest_output", side_effect=OSError("capture failed")):
            with self.assertRaisesRegex(OSError, "capture failed"):
                diagnostic.run_guest(Path("/native"), self.root, self.prepared,
                                     "profile", 1, False, False)
        child.kill.assert_called_once_with()
        child.wait.assert_called_once_with()

    @staticmethod
    def launch_request(path=b"D:\\default.xbe", data=b""):
        return (b"BWRL" + struct.pack("<II", 1, len(path)) + path +
                struct.pack("<I", len(data)) + data)

    def test_relaunch_validation_matches_native_title_and_frame_rules(self):
        for path in (b"default.xbe", b"D:\\default.xbe", b"d:/DEFAULT.XBE",
                     b"\\Device\\CdRom0;default.xbe"):
            for data in (b"", bytes(3072)):
                diagnostic.validate_relaunch(self.launch_request(path, data))

        valid = self.launch_request()
        bad_frames = (
            b"", b"bad!" + valid[4:], valid[:4] + struct.pack("<I", 2) + valid[8:],
            valid[:-1], valid + b"\0", self.launch_request(b"default\0.xbe"),
            self.launch_request(b"x" * 261), self.launch_request(data=b"x"),
        )
        for frame in bad_frames:
            with self.subTest(frame_length=len(frame)), self.assertRaises(RuntimeError):
                diagnostic.validate_relaunch(frame)
        for path in (b"D:\\other.xbe", b"D:\\..\\default.xbe", b"default.xbe/"):
            with self.subTest(path=path), self.assertRaisesRegex(RuntimeError, "another title"):
                diagnostic.validate_relaunch(self.launch_request(path))

    @unittest.skipIf(os.name == "nt", "Windows selectors do not support pipes")
    def test_relaunch_input_is_delivered_as_validated_private_pipe_data(self):
        request = self.launch_request(data=bytes(3072))
        child = mock.Mock()
        child.wait.return_value = 0
        child.poll.return_value = 0

        def spawn(_command, **kwargs):
            input_fd = int(kwargs["env"]["BW_NATIVE_LAUNCH_IN_FD"])
            self.assertEqual(os.read(input_fd, len(request) + 1), request)
            self.assertEqual(os.read(input_fd, 1), b"")
            read_fd, write_fd = os.pipe()
            os.close(write_fd)
            child.stdout = os.fdopen(read_fd, "rb")
            return child

        with mock.patch.object(diagnostic.subprocess, "Popen", side_effect=spawn):
            status, relaunch, log_size = diagnostic.run_guest_once(
                Path("/native"), self.root, self.prepared, "profile", 20, False, False,
                launch=request)
        self.assertEqual((status, relaunch, log_size), (0, b"", 0))

    def test_follow_relaunches_uses_bounded_separate_boots(self):
        request = self.launch_request(data=bytes(3072))
        with mock.patch.object(diagnostic, "run_guest_once",
                               side_effect=[(0, request, 3), (0, b"", 4)]) as once, \
             mock.patch.object(diagnostic.time, "monotonic", side_effect=[10, 11, 12]), \
             contextlib.redirect_stdout(io.StringIO()):
            result = diagnostic.run_guest_relaunches(
                Path("/native"), self.root, self.prepared, "profile", 20, False, True, 4, 30)
        self.assertEqual(result, diagnostic.RelaunchRunResult.guest_exit(0, 2))
        self.assertEqual((self.root / "relaunch-001.bin").read_bytes(), request)
        first, second = once.call_args_list
        self.assertEqual(first.kwargs["launch"], b"")
        self.assertEqual(second.kwargs["launch"], request)
        self.assertEqual(first.kwargs["log_name"], "guest-001.log")
        self.assertEqual(second.kwargs["audit_name"], "gpu-audit-002.txt")
        self.assertEqual(first.kwargs["timeout"], 29)
        self.assertEqual(second.kwargs["timeout"], 28)
        self.assertEqual(second.kwargs["log_limit"], diagnostic.MAX_LOG_BYTES - 3)

    def test_synthetic_input_is_passed_to_each_guest_boot(self):
        request = self.launch_request()
        with mock.patch.object(diagnostic, "run_guest_once",
                               side_effect=[(0, request, 0), (0, b"", 0)]) as once, \
             contextlib.redirect_stdout(io.StringIO()):
            result = diagnostic.run_guest_relaunches(
                Path("/native"), self.root, self.prepared, "profile", 20, False, False,
                4, 30, keyboard_hook=True, synthetic_input=True)
        self.assertEqual(result, diagnostic.RelaunchRunResult.guest_exit(0, 2))
        self.assertEqual(once.call_count, 2)
        for call in once.call_args_list:
            self.assertTrue(call.kwargs["keyboard_hook"])
            self.assertTrue(call.kwargs["synthetic_input"])

    def test_follow_relaunch_rejects_malformed_and_wrong_title_before_next_boot(self):
        for request in (b"bad", self.launch_request(b"D:\\other.xbe")):
            with self.subTest(request=request), \
                 mock.patch.object(diagnostic, "run_guest_once", return_value=(0, request, 0)) as once:
                with self.assertRaises(RuntimeError):
                    diagnostic.run_guest_relaunches(
                        Path("/native"), self.root, self.prepared, "profile", 20, False, False, 4, 30)
                self.assertEqual(once.call_count, 1)
            (self.root / "relaunch-001.bin").unlink()

    def test_follow_relaunch_honors_child_error_boot_limit_and_deadline(self):
        request = self.launch_request()
        with mock.patch.object(diagnostic, "run_guest_once", return_value=(7, request, 0)) as once:
            self.assertEqual(diagnostic.run_guest_relaunches(
                Path("/native"), self.root, self.prepared, "profile", 20, False, False, 4, 30),
                diagnostic.RelaunchRunResult.guest_exit(7, 1))
            self.assertEqual(once.call_count, 1)
        (self.root / "relaunch-001.bin").unlink()
        with mock.patch.object(diagnostic, "run_guest_once", return_value=(0, request, 0)) as once:
            self.assertEqual(
                diagnostic.run_guest_relaunches(
                    Path("/native"), self.root, self.prepared, "profile", 20,
                    False, False, 1, 30),
                diagnostic.RelaunchRunResult.boot_limit(1))
            self.assertEqual(once.call_count, 1)
        with mock.patch.object(diagnostic, "run_guest_once") as once, \
             mock.patch.object(diagnostic.time, "monotonic", side_effect=[10, 40]):
            with self.assertRaisesRegex(RuntimeError, "total diagnostic deadline"):
                diagnostic.run_guest_relaunches(
                    Path("/native"), self.root, self.prepared, "profile", 20, False, False, 4, 30)
            once.assert_not_called()

    def test_numbered_guest_logs_match_completed_boots_exactly(self):
        first = self.root / "guest-001.log"
        second = self.root / "guest-002.log"
        first.write_bytes(b"first complete boot\n")
        second.write_bytes(b"second complete boot\n")
        self.assertEqual(
            diagnostic.guest_log_paths(self.root, True, 2, 4),
            [first, second])

        partial = self.root / "guest-003.log"
        partial.write_bytes(b"partial boot\n")
        with self.assertRaisesRegex(RuntimeError, "missing or out of sequence"):
            diagnostic.guest_log_paths(self.root, True, 2, 4)
        partial.unlink()
        second.unlink()
        with self.assertRaisesRegex(RuntimeError, "missing or out of sequence"):
            diagnostic.guest_log_paths(self.root, True, 2, 4)

    def main_arguments(self):
        executable = self.root / "binary"
        executable.write_bytes(b"synthetic executable")
        executable.chmod(0o700)
        disc = self.prepared / "game/disc"
        disc.mkdir(parents=True)
        (disc / "default.xbe").write_bytes(b"synthetic disc")
        return [
            "--exe", str(executable), "--prepared-tree", str(self.prepared),
            "--toolkit", str(self.toolkit), "--output-dir", str(self.root / "output"),
        ]

    def test_synthetic_cli_requires_keyboard_hook(self):
        arguments = self.main_arguments() + ["--synthetic-input"]
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as failure:
            diagnostic.main(arguments)
        self.assertEqual(failure.exception.code, 2)
        self.assertIn("--synthetic-input requires --experimental-keyboard-hook", errors.getvalue())

    def test_cli_rejects_unknown_or_noncanonical_editions(self):
        base_arguments = self.main_arguments()
        for edition in ("enhanced", "Original", "Remastered"):
            with self.subTest(edition=edition):
                arguments = base_arguments + ["--edition", edition]
                errors = io.StringIO()
                with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
                     contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as failure:
                    diagnostic.main(arguments)
                self.assertEqual(failure.exception.code, 2)
                self.assertIn("invalid choice", errors.getvalue())

    def test_cli_defaults_to_original(self):
        arguments = self.main_arguments()

        def guest(_executable, output, _game_dir, _profile, _seconds, _quincunx, _audit,
                  **kwargs):
            (output / "guest.log").write_bytes(b"no marker summary\n")
            self.assertEqual(kwargs["edition"], "original")
            self.assertFalse(kwargs["disable_audio"])
            return 0

        output = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             mock.patch.object(diagnostic, "ROOT", self.repository), \
             mock.patch.object(diagnostic, "verify_prepared"), \
             mock.patch.object(diagnostic, "toolkit_revision", return_value="pinned"), \
             mock.patch.object(diagnostic, "verify_sandbox"), \
             mock.patch.object(diagnostic, "run_guest", side_effect=guest), \
             contextlib.redirect_stdout(output):
            result = diagnostic.main(arguments)
        self.assertEqual(result, 0)
        self.assertIn("Diagnostic edition: Original", output.getvalue())
        self.assertIn("Diagnostic audio: enabled", output.getvalue())
        self.assertIn("0 received, 0 batch scopes, 0 rejected", output.getvalue())
        self.assertIn("zero observed ocean coverage", output.getvalue())

    def test_cli_audio_disable_is_explicit_and_propagated(self):
        arguments = self.main_arguments() + ["--disable-audio"]

        def guest(_executable, output, _game_dir, _profile, _seconds, _quincunx, _audit,
                  **kwargs):
            (output / "guest.log").write_bytes(b"no marker summary\n")
            self.assertTrue(kwargs["disable_audio"])
            return 0

        output = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             mock.patch.object(diagnostic, "ROOT", self.repository), \
             mock.patch.object(diagnostic, "verify_prepared"), \
             mock.patch.object(diagnostic, "toolkit_revision", return_value="pinned"), \
             mock.patch.object(diagnostic, "verify_sandbox"), \
             mock.patch.object(diagnostic, "run_guest", side_effect=guest), \
             contextlib.redirect_stdout(output):
            result = diagnostic.main(arguments)
        self.assertEqual(result, 0)
        self.assertIn("Diagnostic audio: disabled", output.getvalue())

    def test_relaunch_cap_reports_completed_markers_and_is_non_success(self):
        arguments = self.main_arguments() + [
            "--follow-relaunches", "--max-boots", "2",
        ]

        def capped(_executable, output, *_args, **_kwargs):
            (output / "guest-001.log").write_bytes(
                b"[GPU] ocean draw markers: 5 received, 3 batch scopes, 1 rejected\n")
            (output / "guest-002.log").write_bytes(
                b"[GPU] ocean draw markers: 7 received, 4 batch scopes, 0 rejected\n")
            return diagnostic.RelaunchRunResult.boot_limit(2)

        output = io.StringIO()
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             mock.patch.object(diagnostic, "ROOT", self.repository), \
             mock.patch.object(diagnostic, "verify_prepared"), \
             mock.patch.object(diagnostic, "toolkit_revision", return_value="pinned") as toolkit, \
             mock.patch.object(diagnostic, "snapshot_tree",
                               return_value={"unchanged": 1}) as snapshot, \
             mock.patch.object(diagnostic, "digest_file", return_value="hash") as digest, \
             mock.patch.object(diagnostic, "verify_sandbox"), \
             mock.patch.object(diagnostic, "run_guest_relaunches", side_effect=capped), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = diagnostic.main(arguments)
        self.assertEqual(result, 1)
        self.assertEqual(toolkit.call_count, 2)
        self.assertEqual(snapshot.call_count, 2)
        self.assertEqual(digest.call_count, 2)
        self.assertIn("Prepared-tree, toolkit and executable integrity unchanged",
                      output.getvalue())
        self.assertIn("Ocean markers: 12 received, 7 batch scopes, 1 rejected",
                      output.getvalue())
        self.assertNotIn("Guest exit status:", output.getvalue())
        self.assertIn("--max-boots stopped after 2 completed boots", errors.getvalue())

    def test_relaunch_cap_preserves_malformed_summary_failure(self):
        arguments = self.main_arguments() + [
            "--follow-relaunches", "--max-boots", "1",
        ]

        def capped(_executable, output, *_args, **_kwargs):
            (output / "guest-001.log").write_bytes(
                b"[GPU] ocean draw markers: malformed\n")
            return diagnostic.RelaunchRunResult.boot_limit(1)

        output = io.StringIO()
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             mock.patch.object(diagnostic, "ROOT", self.repository), \
             mock.patch.object(diagnostic, "verify_prepared"), \
             mock.patch.object(diagnostic, "toolkit_revision", return_value="pinned") as toolkit, \
             mock.patch.object(diagnostic, "verify_sandbox"), \
             mock.patch.object(diagnostic, "run_guest_relaunches", side_effect=capped), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = diagnostic.main(arguments)
        self.assertEqual(result, 1)
        self.assertEqual(toolkit.call_count, 2)
        self.assertIn("Prepared-tree, toolkit and executable integrity unchanged",
                      output.getvalue())
        self.assertIn("Diagnostic failed: guest log contains a malformed ocean marker summary",
                      errors.getvalue())
        self.assertNotIn("bounded limit reached", errors.getvalue())

    def test_relaunch_cap_integrity_failure_takes_precedence(self):
        arguments = self.main_arguments() + [
            "--follow-relaunches", "--max-boots", "1",
        ]

        def capped(_executable, output, *_args, **_kwargs):
            (output / "guest-001.log").write_bytes(
                b"[GPU] ocean draw markers: 1 received, 1 batch scopes, 0 rejected\n")
            return diagnostic.RelaunchRunResult.boot_limit(1)

        output = io.StringIO()
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             mock.patch.object(diagnostic, "ROOT", self.repository), \
             mock.patch.object(diagnostic, "verify_prepared"), \
             mock.patch.object(diagnostic, "toolkit_revision", return_value="pinned"), \
             mock.patch.object(diagnostic, "snapshot_tree", return_value={"unchanged": 1}), \
             mock.patch.object(diagnostic, "digest_file",
                               side_effect=["before", "after"]), \
             mock.patch.object(diagnostic, "verify_sandbox"), \
             mock.patch.object(diagnostic, "run_guest_relaunches", side_effect=capped), \
             mock.patch.object(diagnostic, "report_ocean_marker_counts") as report, \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = diagnostic.main(arguments)
        self.assertEqual(result, 1)
        report.assert_not_called()
        self.assertIn("Diagnostic failed: toolkit or executable changed", errors.getvalue())
        self.assertNotIn("bounded limit reached", errors.getvalue())

    def test_ordinary_relaunch_completion_reports_success(self):
        arguments = self.main_arguments() + [
            "--follow-relaunches", "--max-boots", "3",
        ]

        def completed(_executable, output, *_args, **_kwargs):
            (output / "guest-001.log").write_bytes(
                b"[GPU] ocean draw markers: 2 received, 1 batch scopes, 0 rejected\n")
            (output / "guest-002.log").write_bytes(
                b"[GPU] ocean draw markers: 3 received, 2 batch scopes, 0 rejected\n")
            return diagnostic.RelaunchRunResult.guest_exit(0, 2)

        output = io.StringIO()
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             mock.patch.object(diagnostic, "ROOT", self.repository), \
             mock.patch.object(diagnostic, "verify_prepared"), \
             mock.patch.object(diagnostic, "toolkit_revision", return_value="pinned"), \
             mock.patch.object(diagnostic, "verify_sandbox"), \
             mock.patch.object(diagnostic, "run_guest_relaunches", side_effect=completed), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = diagnostic.main(arguments)
        self.assertEqual(result, 0)
        self.assertEqual(errors.getvalue(), "")
        self.assertIn("Ocean markers: 5 received, 3 batch scopes, 0 rejected",
                      output.getvalue())
        self.assertIn("Guest exit status: 0", output.getvalue())

    def test_relaunch_run_limit_preserves_guest_status(self):
        arguments = self.main_arguments() + [
            "--follow-relaunches", "--max-boots", "3",
        ]

        def limited(_executable, output, *_args, **_kwargs):
            (output / "guest-001.log").write_bytes(b"configured run limit\n")
            return diagnostic.RelaunchRunResult.guest_exit(124, 1)

        output = io.StringIO()
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             mock.patch.object(diagnostic, "ROOT", self.repository), \
             mock.patch.object(diagnostic, "verify_prepared"), \
             mock.patch.object(diagnostic, "toolkit_revision", return_value="pinned"), \
             mock.patch.object(diagnostic, "verify_sandbox"), \
             mock.patch.object(diagnostic, "run_guest_relaunches", side_effect=limited), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = diagnostic.main(arguments)
        self.assertEqual(result, 124)
        self.assertEqual(errors.getvalue(), "")
        self.assertIn("Guest exit status: 124", output.getvalue())

    def test_missing_ocean_marker_summary_reports_zero_coverage(self):
        log = self.root / "guest.log"
        log.write_bytes(b"unrelated guest output\n")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            counts = diagnostic.report_ocean_marker_counts([log])
        self.assertEqual(counts, (0, 0, 0))
        self.assertIn("no marker summary; zero observed ocean coverage", output.getvalue())

    def test_malformed_ocean_marker_summaries_are_rejected(self):
        log = self.root / "guest.log"
        malformed = (
            b"[GPU] ocean draw markers: 1 received, missing fields\n",
            b"[GPU] ocean draw markers: 18446744073709551616 received, "
            b"1 batch scopes, 0 rejected\n",
            b"[GPU] ocean draw markers: 2 received, 2 batch scopes, 0 rejected\n"
            b"[GPU] ocean draw markers: 1 received, 1 batch scopes, 0 rejected\n",
        )
        for contents in malformed:
            with self.subTest(contents=contents):
                log.write_bytes(contents)
                with self.assertRaisesRegex(RuntimeError, "malformed|out-of-range|non-monotonic"):
                    diagnostic.extract_ocean_marker_counts([log])

    def test_ocean_marker_summaries_use_each_boots_final_bounded_counts(self):
        first = self.root / "guest-001.log"
        second = self.root / "guest-002.log"
        first.write_bytes(
            b"private output is ignored\n"
            b"[GPU] ocean draw markers: 8 received, 6 batch scopes, 1 rejected\n"
            b"[GPU] ocean draw markers: 13 received, 10 batch scopes, 2 rejected\r\n")
        second.write_bytes(
            b"[GPU] ocean draw markers: 3 received, 2 batch scopes, 0 rejected\n")
        self.assertEqual(
            diagnostic.extract_ocean_marker_counts([first, second]),
            ((16, 12, 2), 3))
        with mock.patch.object(diagnostic, "MAX_OCEAN_MARKER_SUMMARIES", 2):
            with self.assertRaisesRegex(RuntimeError, "too many"):
                diagnostic.extract_ocean_marker_counts([first, second])

    def test_preflight_failure_never_launches_guest_and_rechecks_integrity(self):
        arguments = self.main_arguments()
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             mock.patch.object(diagnostic, "ROOT", self.repository), \
             mock.patch.object(diagnostic, "verify_prepared"), \
             mock.patch.object(diagnostic, "toolkit_revision", return_value="pinned") as toolkit, \
             mock.patch.object(diagnostic, "verify_sandbox", side_effect=RuntimeError("denied")), \
             mock.patch.object(diagnostic, "run_guest") as guest, \
             contextlib.redirect_stderr(errors), contextlib.redirect_stdout(io.StringIO()):
            result = diagnostic.main(arguments)
        self.assertEqual(result, 1)
        guest.assert_not_called()
        self.assertEqual(toolkit.call_count, 2)
        self.assertIn("Diagnostic failed: denied", errors.getvalue())
        self.assertIn("Private diagnostic output:", errors.getvalue())

    def test_integrity_mismatch_overrides_a_successful_guest_exit(self):
        arguments = self.main_arguments()
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             mock.patch.object(diagnostic, "ROOT", self.repository), \
             mock.patch.object(diagnostic, "verify_prepared"), \
             mock.patch.object(diagnostic, "toolkit_revision", return_value="pinned"), \
             mock.patch.object(diagnostic, "snapshot_tree", side_effect=[{"before": 1}, {"after": 1}]), \
             mock.patch.object(diagnostic, "verify_sandbox"), \
             mock.patch.object(diagnostic, "run_guest", return_value=0), \
             contextlib.redirect_stderr(errors), contextlib.redirect_stdout(io.StringIO()):
            result = diagnostic.main(arguments)
        self.assertEqual(result, 1)
        self.assertIn("prepared tree changed during the diagnostic", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
