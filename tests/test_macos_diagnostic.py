import contextlib
import importlib.util
import io
import os
from pathlib import Path
import stat
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
                                         "BW_SETTINGS": "unsafe"}, clear=True):
            env = diagnostic.diagnostic_env(self.root, self.prepared, 15, False, False, 4, 5)
        self.assertNotIn("RECOMP_USB", env)
        self.assertNotIn("RECOMP_AC97_READY", env)
        self.assertNotIn("BW_METAL_QUINCUNX_APPROX", env)
        self.assertNotIn("BW_GPU_AUDIT", env)
        self.assertEqual(env["BW_SETTINGS"], "none")
        self.assertEqual(env["BW_RUN_SECONDS"], "15")
        self.assertEqual(env["BW_NATIVE_LAUNCH_IN_FD"], "4")
        enabled = diagnostic.diagnostic_env(self.root, self.prepared, 15, True, True, 4, 5)
        self.assertEqual(enabled["BW_METAL_QUINCUNX_APPROX"], "1")
        self.assertEqual(enabled["BW_GPU_AUDIT"], "1")
        self.assertEqual(enabled["BW_GPU_AUDIT_FILE"], str(self.root / "gpu-audit.txt"))

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
