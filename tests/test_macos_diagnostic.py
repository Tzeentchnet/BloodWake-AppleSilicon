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
                                         "RECOMP_USB_PADS": "4",
                                         "RECOMP_PAD_LIVE": "/unsafe",
                                         "RECOMP_PB_REPORT_MS": "1",
                                         "BW_MACOS_AUDIO": "0",
                                         "BW_EDITION": "remastered",
                                         "BW_SETTINGS": "unsafe",
                                         "BW_MACOS_KEYBOARD_HOOK": "1",
                                         "BW_MACOS_GUIDED_INPUT": "/unsafe",
                                         "BW_SYNTHETIC_INPUT_DIAGNOSTIC": "1",
                                         "BW_SYNTHETIC_INPUT_SCRIPT": "unsafe",
                                         "BW_METAL_WATER": "realistic",
                                         "BW_METAL_WATER_BENCHMARK": "1",
                                         "BW_METAL_WATER_CAPTURE_TRIGGER": "/unsafe",
                                         "BW_METAL_PIPELINE_WARMUP": "/unsafe",
                                         "BW_METAL_PIPELINE_TRACE_OUTPUT": "/unsafe",
                                         "RECOMP_FB_DUMP": "/unsafe",
                                         "RECOMP_FB_DUMP_FLIPS": "1",
                                         "BW_HEADLESS_AUDIO_AUDIT": "1",
                                         "BW_NATIVE_DIAGNOSTIC": "unsafe",
                                         "RECOMP_AUDIO_HEADLESS_AUDIT": "1"}, clear=True):
            env = diagnostic.diagnostic_env(self.root, self.prepared, 15, False, False, 4, 5)
        self.assertNotIn("RECOMP_USB", env)
        self.assertNotIn("RECOMP_USB_PADS", env)
        self.assertNotIn("RECOMP_PAD_LIVE", env)
        self.assertNotIn("RECOMP_PB_REPORT_MS", env)
        self.assertNotIn("RECOMP_AC97_READY", env)
        self.assertNotIn("BW_MACOS_AUDIO", env)
        self.assertNotIn("BW_METAL_QUINCUNX_APPROX", env)
        self.assertNotIn("BW_GPU_AUDIT", env)
        self.assertNotIn("BW_MACOS_KEYBOARD_HOOK", env)
        self.assertNotIn("BW_MACOS_GUIDED_INPUT", env)
        self.assertNotIn("BW_SYNTHETIC_INPUT_DIAGNOSTIC", env)
        self.assertNotIn("BW_SYNTHETIC_INPUT_SCRIPT", env)
        self.assertNotIn("BW_HEADLESS_AUDIO_AUDIT", env)
        self.assertNotIn("RECOMP_AUDIO_HEADLESS_AUDIT", env)
        self.assertNotIn("BW_METAL_WATER", env)
        self.assertNotIn("BW_METAL_WATER_BENCHMARK", env)
        self.assertNotIn("BW_METAL_WATER_CAPTURE_TRIGGER", env)
        self.assertNotIn("BW_METAL_PIPELINE_WARMUP", env)
        self.assertNotIn("BW_METAL_PIPELINE_TRACE_OUTPUT", env)
        self.assertNotIn("RECOMP_FB_DUMP", env)
        self.assertNotIn("RECOMP_FB_DUMP_FLIPS", env)
        self.assertEqual(env["BW_SETTINGS"], "none")
        self.assertEqual(env["BW_EDITION"], "original")
        self.assertEqual(env["BW_RUN_SECONDS"], "15")
        self.assertEqual(env["BW_NATIVE_DIAGNOSTIC"], "1")
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
        scripted = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5, keyboard_hook=True,
            edition="remastered",
            synthetic_input_script="remastered-boot-menu-to-gameplay-v1")
        self.assertEqual(scripted["BW_SYNTHETIC_INPUT_SCRIPT"],
                         "remastered-boot-menu-to-gameplay-v1")
        self.assertNotIn("BW_SYNTHETIC_INPUT_DIAGNOSTIC", scripted)
        split_scripted = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5, keyboard_hook=True,
            edition="remastered",
            synthetic_input_script="remastered-boot-menu-to-gameplay-2p-v1")
        self.assertEqual(split_scripted["BW_SYNTHETIC_INPUT_SCRIPT"],
                         "remastered-boot-menu-to-gameplay-2p-v1")
        remastered = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5, edition="remastered")
        self.assertEqual(remastered["BW_EDITION"], "remastered")
        realistic = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5,
            edition="remastered", water_preset="realistic")
        self.assertEqual(realistic["BW_METAL_WATER"], "realistic")
        benchmark = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, True, 4, 5,
            edition="remastered", water_benchmark=True)
        self.assertEqual(benchmark["BW_METAL_WATER_BENCHMARK"], "1")
        self.assertNotIn("BW_METAL_WATER", benchmark)
        capture = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, True, 4, 5,
            keyboard_hook=True, edition="remastered",
            synthetic_input_script="remastered-boot-menu-to-gameplay-v1",
            water_capture=True, capture_stem="water-capture-002")
        self.assertEqual(
            capture["RECOMP_FB_DUMP"],
            str(self.root / "water-capture-002-")
        )
        self.assertEqual(
            capture["RECOMP_FB_DUMP_FLIPS"],
            str(self.root / ".water-capture-002.trigger")
        )
        self.assertEqual(
            capture["BW_METAL_WATER_CAPTURE_TRIGGER"],
            capture["RECOMP_FB_DUMP_FLIPS"]
        )
        guided = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5,
            guided_input=True)
        self.assertEqual(guided["BW_MACOS_KEYBOARD_HOOK"], "1")
        self.assertEqual(guided["BW_MACOS_GUIDED_INPUT"],
                         str(self.root / "guided-pad.txt"))
        self.assertEqual(guided["RECOMP_FB_DUMP"],
                         str(self.root / "guided-frame-"))
        guided_scripted = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5,
            keyboard_hook=True, guided_input=True, edition="remastered",
            synthetic_input_script="remastered-boot-menu-to-gameplay-v1")
        self.assertEqual(
            guided_scripted["BW_SYNTHETIC_INPUT_SCRIPT"],
            "remastered-boot-menu-to-gameplay-v1")
        self.assertEqual(guided_scripted["BW_MACOS_GUIDED_INPUT"],
                         str(self.root / "guided-pad.txt"))
        self.assertEqual(guided["RECOMP_PB_REPORT_MS"], "2000")
        audio_disabled = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5,
            disable_audio=True)
        self.assertEqual(audio_disabled["BW_MACOS_AUDIO"], "0")
        audio_audit = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5,
            audio_audit=True)
        self.assertEqual(audio_audit["BW_HEADLESS_AUDIO_AUDIT"], "1")
        self.assertEqual(audio_audit["BW_NATIVE_DIAGNOSTIC"], "1")
        self.assertNotIn("BW_MACOS_AUDIO", audio_audit)
        self.assertNotIn("RECOMP_AUDIO_HEADLESS_AUDIT", audio_audit)
        performance = diagnostic.diagnostic_env(
            self.root, self.prepared, 15, False, False, 4, 5,
            frame_timing=True, pipeline_timing=True,
            profile_name="profile-002.txt",
            profile_from=20.0, profile_to=40.0, profile_callers=True,
            render_scale="2.25", frame_cap=60,
            pipeline_trace_name="pipeline-trace-002.bwmpa",
            pipeline_warmup=self.root / "warmup.bwmpa")
        self.assertEqual(performance["BW_METAL_FRAME_TIMING"], "1")
        self.assertEqual(performance["BW_METAL_PIPELINE_TIMING"], "1")
        self.assertEqual(performance["BW_PROFILE"],
                         str(self.root / "profile-002.txt"))
        self.assertEqual(performance["BW_PROFILE_FROM"], "20.0")
        self.assertEqual(performance["BW_PROFILE_TO"], "40.0")
        self.assertEqual(performance["BW_PROFILE_CALLERS"], "1")
        self.assertEqual(performance["BW_METAL_RENDER_SCALE"], "2.25")
        self.assertEqual(performance["BW_HIFPS"], "60")
        self.assertEqual(
            performance["BW_METAL_PIPELINE_TRACE_OUTPUT"],
            str(self.root / "pipeline-trace-002.bwmpa")
        )
        self.assertEqual(
            performance["BW_METAL_PIPELINE_WARMUP"],
            str(self.root / "warmup.bwmpa")
        )
        with self.assertRaisesRegex(ValueError, "requires audio to remain enabled"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5,
                disable_audio=True, audio_audit=True)
        with self.assertRaisesRegex(ValueError, "trace output name is invalid"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5,
                pipeline_trace_name="../trace.bwmpa")
        with self.assertRaisesRegex(ValueError, "absolute .bwmpa"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5,
                pipeline_warmup="relative.bwmpa")
        with self.assertRaisesRegex(ValueError, "edition must be original or remastered"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5, edition="Remastered")
        with self.assertRaisesRegex(ValueError, "requires the experimental keyboard hook"):
            diagnostic.diagnostic_env(self.root, self.prepared, 15, False, False, 4, 5,
                                      synthetic_input=True)
        with self.assertRaisesRegex(ValueError, "unknown synthetic input script"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5,
                keyboard_hook=True, edition="remastered",
                synthetic_input_script="unknown")
        with self.assertRaisesRegex(ValueError, "requires the remastered edition"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5,
                keyboard_hook=True,
                synthetic_input_script="remastered-boot-menu-to-gameplay-v1")
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5,
                keyboard_hook=True, synthetic_input=True, edition="remastered",
                synthetic_input_script="remastered-boot-menu-to-gameplay-v1")
        with self.assertRaisesRegex(ValueError, "requires the remastered edition"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, True, 4, 5,
                water_benchmark=True)
        with self.assertRaisesRegex(ValueError, "requires the retail water preset"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, True, 4, 5,
                edition="remastered", water_preset="realistic",
                water_benchmark=True)
        with self.assertRaisesRegex(ValueError, "requires GPU audit"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5,
                edition="remastered", water_benchmark=True)
        with self.assertRaisesRegex(ValueError, "requires the remastered edition"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, True, 4, 5,
                keyboard_hook=True,
                synthetic_input_script="remastered-boot-menu-to-gameplay-v1",
                water_capture=True)
        with self.assertRaisesRegex(ValueError, "requires GPU audit"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5,
                keyboard_hook=True, edition="remastered",
                synthetic_input_script="remastered-boot-menu-to-gameplay-v1",
                water_capture=True)
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, True, 4, 5,
                keyboard_hook=True, edition="remastered",
                synthetic_input_script="remastered-boot-menu-to-gameplay-v1",
                water_benchmark=True, water_capture=True)
        with self.assertRaisesRegex(ValueError, "deterministic synthetic input"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, True, 4, 5,
                keyboard_hook=True, edition="remastered", water_capture=True)
        with self.assertRaisesRegex(ValueError, "experimental keyboard input path"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, False, 4, 5,
                keyboard_hook=True, guided_input=True)
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            diagnostic.diagnostic_env(
                self.root, self.prepared, 15, False, True, 4, 5,
                edition="remastered", water_capture=True, guided_input=True)

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

    def test_synthetic_script_is_passed_to_each_guest_boot(self):
        request = self.launch_request()
        script = "remastered-boot-menu-to-gameplay-v1"
        with mock.patch.object(diagnostic, "run_guest_once",
                               side_effect=[(0, request, 0), (0, b"", 0)]) as once, \
             contextlib.redirect_stdout(io.StringIO()):
            result = diagnostic.run_guest_relaunches(
                Path("/native"), self.root, self.prepared, "profile", 20, False, False,
                4, 30, keyboard_hook=True, edition="remastered",
                synthetic_input_script=script)
        self.assertEqual(result, diagnostic.RelaunchRunResult.guest_exit(0, 2))
        for call in once.call_args_list:
            self.assertTrue(call.kwargs["keyboard_hook"])
            self.assertEqual(call.kwargs["synthetic_input_script"], script)

    def test_water_capture_uses_boot_scoped_private_prefixes(self):
        request = self.launch_request()
        script = "remastered-boot-menu-to-gameplay-v1"
        with mock.patch.object(
            diagnostic, "run_guest_once",
            side_effect=[(0, request, 0), (0, b"", 0)]
        ) as once, contextlib.redirect_stdout(io.StringIO()):
            result = diagnostic.run_guest_relaunches(
                Path("/native"), self.root, self.prepared, "profile", 20,
                False, True, 4, 30, keyboard_hook=True,
                edition="remastered", synthetic_input_script=script,
                water_capture=True
            )
        self.assertEqual(result, diagnostic.RelaunchRunResult.guest_exit(0, 2))
        self.assertEqual(
            [call.kwargs["capture_stem"] for call in once.call_args_list],
            ["water-capture-001", "water-capture-002"]
        )
        self.assertTrue(all(
            call.kwargs["water_capture"] for call in once.call_args_list
        ))

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

    def test_realistic_water_coverage_is_boot_bounded(self):
        first = self.root / "guest-001.log"
        second = self.root / "guest-002.log"
        first.write_bytes(diagnostic.REALISTIC_WATER_ACTIVE + b"\n")
        second.write_bytes(
            diagnostic.REALISTIC_WATER_ACTIVE + b"\n" +
            diagnostic.REALISTIC_WATER_FALLBACK_PREFIX + b"(synthetic)\n"
        )
        self.assertEqual(
            diagnostic.extract_realistic_water_coverage([first, second]), (2, 1))
        with self.assertRaisesRegex(RuntimeError, "retail preset"):
            diagnostic.report_realistic_water_coverage([first], "retail")

    def test_realistic_water_coverage_requires_activation(self):
        log = self.root / "guest.log"
        log.write_bytes(diagnostic.REALISTIC_WATER_FALLBACK_PREFIX + b"(synthetic)\n")
        with self.assertRaisesRegex(RuntimeError, "did not activate"):
            diagnostic.report_realistic_water_coverage([log], "realistic")

    @staticmethod
    def water_benchmark_summary():
        return (
            b"[GPU-METAL] water benchmark v1 "
            b"retail_samples=100 retail_mean_ms=1.000000 "
            b"retail_median_ms=1.000000 retail_p95_ms=1.000000 "
            b"realistic_samples=100 realistic_mean_ms=1.100000 "
            b"realistic_median_ms=1.200000 realistic_p95_ms=1.300000 "
            b"mean_delta_percent=+10.000000 "
            b"median_delta_percent=+20.000000 "
            b"p95_delta_percent=+30.000000"
        )

    @staticmethod
    def water_capture_summary(preset="retail", width=2, height=1):
        return (
            b"[GPU-METAL] water capture v1 preset=" +
            preset.encode("ascii") +
            b" images=4 supported_offsets=0,60,120,180 width=" +
            str(width).encode("ascii") + b" height=" +
            str(height).encode("ascii")
        )

    @staticmethod
    def write_capture_bitmap(path, width=2, height=1):
        row_bytes = (width * 3 + 3) & ~3
        image_bytes = row_bytes * height
        header = bytearray(54)
        header[:2] = b"BM"
        struct.pack_into("<I", header, 2, 54 + image_bytes)
        struct.pack_into("<I", header, 10, 54)
        struct.pack_into("<III", header, 14, 40, width, height)
        struct.pack_into("<HH", header, 26, 1, 24)
        struct.pack_into("<I", header, 34, image_bytes)
        path.write_bytes(header + bytes(image_bytes))

    def test_water_benchmark_summary_is_strict_and_reported(self):
        log = self.root / "guest.log"
        log.write_bytes(self.water_benchmark_summary() + b"\n")
        benchmarks = diagnostic.read_water_benchmarks([log])
        self.assertEqual(len(benchmarks), 1)
        self.assertEqual(benchmarks[0]["retail_samples"], 100)
        self.assertEqual(benchmarks[0]["delta"], (10.0, 20.0, 30.0))

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            diagnostic.report_water_benchmarks([log], True)
        self.assertIn("1 completed boot(s), 200 measured", output.getvalue())
        self.assertIn("mean +10.000%, median +20.000%, p95 +30.000%",
                      output.getvalue())
        with self.assertRaisesRegex(RuntimeError, "without --water-benchmark"):
            diagnostic.report_water_benchmarks([log], False)

    def test_water_benchmark_summary_rejects_invalid_evidence(self):
        log = self.root / "guest.log"
        valid = self.water_benchmark_summary()
        invalid = (
            diagnostic.WATER_BENCHMARK_PREFIX + b"malformed\n",
            valid + b" trailing-data\n",
            valid.replace(b"retail_samples=100", b"retail_samples=99") + b"\n",
            valid.replace(b"retail_mean_ms=1.000000",
                          b"retail_mean_ms=nan") + b"\n",
            valid.replace(b"mean_delta_percent=+10.000000",
                          b"mean_delta_percent=+11.000000") + b"\n",
            valid + b"\n" + valid + b"\n",
        )
        for evidence in invalid:
            with self.subTest(evidence=evidence):
                log.write_bytes(evidence)
                with self.assertRaisesRegex(
                    RuntimeError, "water benchmark"
                ):
                    diagnostic.read_water_benchmarks([log])
        log.write_bytes(b"no water benchmark\n")
        with self.assertRaisesRegex(RuntimeError, "no boot completed"):
            diagnostic.report_water_benchmarks([log], True)

    def test_water_benchmark_coverage_accepts_controlled_activation(self):
        log = self.root / "guest.log"
        log.write_bytes(diagnostic.REALISTIC_WATER_ACTIVE + b"\n")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = diagnostic.report_realistic_water_coverage(
                [log], "retail", True
            )
        self.assertEqual(result, (1, 0))
        self.assertIn("benchmark", output.getvalue())

    def test_water_capture_sequence_is_private_bounded_and_reported(self):
        first = self.root / "guest-001.log"
        second = self.root / "guest-002.log"
        first.write_bytes(b"no supported water in this boot\n")
        second.write_bytes(self.water_capture_summary() + b"\n")
        for image in range(3):
            self.write_capture_bitmap(
                self.root / f"water-capture-001-{image:05d}.bmp"
            )
        for image in range(4):
            self.write_capture_bitmap(
                self.root / f"water-capture-002-{image:05d}.bmp"
            )
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            captures = diagnostic.report_water_captures(
                [first, second], self.root, True, "retail", True
            )
        self.assertEqual(len(captures), 1)
        self.assertEqual(captures[0]["width"], 2)
        self.assertIn("4 private retail frames each at 2x1", output.getvalue())
        self.assertIn("1 partial boot sequence(s) validated and excluded",
                      output.getvalue())
        with self.assertRaisesRegex(RuntimeError, "without --water-capture"):
            diagnostic.report_water_captures(
                [first, second], self.root, False, "retail", True
            )

    def test_water_capture_parser_rejects_incomplete_or_malformed_evidence(self):
        log = self.root / "guest.log"
        log.write_bytes(self.water_capture_summary() + b"\n")
        for image in range(3):
            self.write_capture_bitmap(
                self.root / f"water-capture-{image:05d}.bmp"
            )
        with self.assertRaisesRegex(RuntimeError, "sequence is incomplete"):
            diagnostic.report_water_captures(
                [log], self.root, True, "retail", False
            )
        self.write_capture_bitmap(self.root / "water-capture-00003.bmp")
        (self.root / "water-capture-00002.bmp").write_bytes(b"not a bitmap")
        with self.assertRaisesRegex(RuntimeError, "invalid BMP header"):
            diagnostic.report_water_captures(
                [log], self.root, True, "retail", False
            )
        log.write_bytes(
            self.water_capture_summary() + b"\n" +
            self.water_capture_summary() + b"\n"
        )
        with self.assertRaisesRegex(RuntimeError, "duplicate water capture"):
            diagnostic.report_water_captures(
                [log], self.root, True, "retail", False
            )
        log.write_bytes(diagnostic.WATER_CAPTURE_PREFIX + b"malformed\n")
        with self.assertRaisesRegex(RuntimeError, "malformed water capture"):
            diagnostic.report_water_captures(
                [log], self.root, True, "retail", False
            )

    def test_water_capture_requires_a_completed_boot(self):
        log = self.root / "guest.log"
        log.write_bytes(b"no capture\n")
        with self.assertRaisesRegex(RuntimeError, "no boot completed"):
            diagnostic.report_water_captures(
                [log], self.root, True, "realistic", False
            )

    def test_water_capture_rejects_mismatched_extra_or_unsafe_evidence(self):
        log = self.root / "guest.log"
        for image in range(4):
            self.write_capture_bitmap(
                self.root / f"water-capture-{image:05d}.bmp"
            )
        log.write_bytes(self.water_capture_summary("realistic") + b"\n")
        with self.assertRaisesRegex(RuntimeError, "preset is inconsistent"):
            diagnostic.report_water_captures(
                [log], self.root, True, "retail", False
            )
        log.write_bytes(self.water_capture_summary(width=3) + b"\n")
        with self.assertRaisesRegex(RuntimeError, "metadata is inconsistent"):
            diagnostic.report_water_captures(
                [log], self.root, True, "retail", False
            )
        log.write_bytes(self.water_capture_summary() + b"\n")
        trigger = self.root / ".water-capture.trigger"
        trigger.write_bytes(b"")
        with self.assertRaisesRegex(RuntimeError, "trigger remained"):
            diagnostic.report_water_captures(
                [log], self.root, True, "retail", False
            )
        trigger.unlink()
        extra = self.root / "water-capture-00004.bmp"
        self.write_capture_bitmap(extra)
        with self.assertRaisesRegex(RuntimeError, "sequence is incomplete"):
            diagnostic.report_water_captures(
                [log], self.root, True, "retail", False
            )
        extra.unlink()
        last = self.root / "water-capture-00003.bmp"
        last.unlink()
        last.symlink_to(self.root / "water-capture-00000.bmp")
        with self.assertRaisesRegex(RuntimeError, "regular file"):
            diagnostic.report_water_captures(
                [log], self.root, True, "retail", False
            )

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

    def test_audio_audit_cli_rejects_disabled_audio(self):
        arguments = self.main_arguments() + [
            "--headless-audio-audit", "--disable-audio",
        ]
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as failure:
            diagnostic.main(arguments)
        self.assertEqual(failure.exception.code, 2)
        self.assertIn("mutually exclusive", errors.getvalue())

    def test_realistic_water_cli_requires_remastered(self):
        arguments = self.main_arguments() + ["--water-preset", "realistic"]
        errors = io.StringIO()
        with mock.patch.object(diagnostic.sys, "platform", "darwin"), \
             contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as failure:
            diagnostic.main(arguments)
        self.assertEqual(failure.exception.code, 2)
        self.assertIn("requires --edition remastered", errors.getvalue())

    def test_water_benchmark_cli_requires_controlled_mode(self):
        arguments = self.main_arguments()
        cases = (
            (["--water-benchmark"],
             "--water-benchmark requires --edition remastered"),
            (["--water-benchmark", "--edition", "remastered"],
             "--water-benchmark requires --gpu-audit"),
            (["--water-benchmark", "--edition", "remastered", "--gpu-audit",
              "--water-preset", "realistic"],
             "--water-benchmark requires --water-preset retail"),
        )
        for extra, expected in cases:
            errors = io.StringIO()
            with self.subTest(extra=extra), \
                 mock.patch.object(diagnostic.sys, "platform", "darwin"), \
                 contextlib.redirect_stderr(errors), \
                 self.assertRaises(SystemExit) as failure:
                diagnostic.main(arguments + extra)
            self.assertEqual(failure.exception.code, 2)
            self.assertIn(expected, errors.getvalue())

    def test_water_capture_cli_requires_controlled_mode(self):
        script = "remastered-boot-menu-to-gameplay-v1"
        arguments = self.main_arguments()
        cases = (
            (["--water-capture"],
             "--water-capture requires --edition remastered"),
            (["--water-capture", "--edition", "remastered"],
             "--water-capture requires --gpu-audit"),
            (["--water-capture", "--edition", "remastered", "--gpu-audit"],
             "--water-capture requires the deterministic"),
            (["--water-capture", "--water-benchmark", "--edition", "remastered",
              "--gpu-audit", "--experimental-keyboard-hook",
              "--synthetic-input-script", script],
             "--water-capture and --water-benchmark are mutually exclusive"),
        )
        for extra, expected in cases:
            errors = io.StringIO()
            with self.subTest(extra=extra), \
                 mock.patch.object(diagnostic.sys, "platform", "darwin"), \
                 contextlib.redirect_stderr(errors), \
                 self.assertRaises(SystemExit) as failure:
                diagnostic.main(arguments + extra)
            self.assertEqual(failure.exception.code, 2)
            self.assertIn(expected, errors.getvalue())

    def test_script_cli_requires_hook_remastered_and_known_name(self):
        script = "remastered-boot-menu-to-gameplay-v1"
        arguments = self.main_arguments()
        cases = [
            (["--synthetic-input-script", script, "--edition", "remastered"],
             "--synthetic-input-script requires --experimental-keyboard-hook"),
            (["--synthetic-input-script", script, "--experimental-keyboard-hook"],
             "--synthetic-input-script requires --edition remastered"),
            (["--synthetic-input-script", "unknown", "--experimental-keyboard-hook",
              "--edition", "remastered"], "invalid choice"),
        ]
        for extra, expected in cases:
            errors = io.StringIO()
            with self.subTest(extra=extra), \
                 mock.patch.object(diagnostic.sys, "platform", "darwin"), \
                 contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as failure:
                diagnostic.main(arguments + extra)
            self.assertEqual(failure.exception.code, 2)
            self.assertIn(expected, errors.getvalue())

    def test_fingerprint_counts_are_aggregated_without_reporting_hashes(self):
        empty = self.root / "empty-audit"
        empty.mkdir()
        self.assertEqual(diagnostic.gpu_audit_paths(empty, True, 2, 4), [])
        no_audit = io.StringIO()
        with contextlib.redirect_stdout(no_audit):
            diagnostic.report_ocean_fingerprint_counts([])
        self.assertIn("zero observed fingerprint coverage", no_audit.getvalue())

        first = self.root / "gpu-audit-001.txt"
        second = self.root / "gpu-audit-002.txt"
        first.write_bytes(
            diagnostic.OCEAN_FINGERPRINT_HEADER + b"\n"
            b"  batches 7, hashes 2/32, table-overflow batches 0, "
            b"within-scope mismatches 0\n"
            b"  1111111111111111          3 batches\n"
            b"  2222222222222222          4 batches\n")
        second.write_bytes(
            diagnostic.OCEAN_FINGERPRINT_HEADER + b"\n"
            b"  batches 9, hashes 2/32, table-overflow batches 1, "
            b"within-scope mismatches 2\n"
            b"  2222222222222222          5 batches\n"
            b"  3333333333333333          3 batches\n")
        self.assertEqual(
            diagnostic.gpu_audit_paths(self.root, True, 2, 4),
            [first, second])
        self.assertEqual(
            diagnostic.extract_ocean_fingerprint_counts([first, second]),
            (16, 3, 1, 2))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            diagnostic.report_ocean_fingerprint_counts([first, second])
        self.assertIn("16 batches, 3 distinct hashes", output.getvalue())
        self.assertNotIn("1111111111111111", output.getvalue())

    def test_fingerprint_parser_rejects_malformed_or_inconsistent_evidence(self):
        audit = self.root / "gpu-audit.txt"
        prefix = diagnostic.OCEAN_FINGERPRINT_HEADER + b"\n"
        invalid_sections = (
            b"  batches 1, hashes 1/32, table-overflow batches 0, "
            b"within-scope mismatches 0\nmalformed row\n",
            b"  batches 2, hashes 1/32, table-overflow batches 0, "
            b"within-scope mismatches 0\n"
            b"  1111111111111111          1 batches\n",
            b"  batches 1, hashes 1/32, table-overflow batches 0, "
            b"within-scope mismatches 2\n"
            b"  1111111111111111          1 batches\n",
            b"  1111111111111111          1 batches\n"
            b"  batches 1, hashes 1/32, table-overflow batches 0, "
            b"within-scope mismatches 0\n",
        )
        for section in invalid_sections:
            with self.subTest(section=section):
                audit.write_bytes(prefix + section)
                with self.assertRaisesRegex(RuntimeError, "ocean fingerprint"):
                    diagnostic.extract_ocean_fingerprint_counts([audit])

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
            self.assertFalse(kwargs["audio_audit"])
            self.assertFalse(kwargs["water_benchmark"])
            self.assertFalse(kwargs["water_capture"])
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

    def test_water_benchmark_cli_propagates_and_reports(self):
        arguments = self.main_arguments() + [
            "--water-benchmark", "--edition", "remastered", "--gpu-audit",
        ]

        def guest(_executable, output, _game_dir, _profile, _seconds,
                  _quincunx, _audit, **kwargs):
            self.assertTrue(kwargs["water_benchmark"])
            self.assertEqual(kwargs["water_preset"], "retail")
            (output / "guest.log").write_bytes(
                b"[GPU] ocean draw markers: 100 received, 100 batch scopes, "
                b"0 rejected\n" +
                diagnostic.REALISTIC_WATER_ACTIVE + b"\n" +
                self.water_benchmark_summary() + b"\n"
            )
            (output / "gpu-audit.txt").write_bytes(
                diagnostic.OCEAN_FINGERPRINT_HEADER + b"\n"
                b"  batches 0, hashes 0/32, table-overflow batches 0, "
                b"within-scope mismatches 0\n"
            )
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
        self.assertIn("Retail/Realistic/Realistic/Retail", output.getvalue())
        self.assertIn("Water benchmark: 1 completed boot(s)", output.getvalue())

    def test_water_capture_cli_propagates_and_reports(self):
        arguments = self.main_arguments() + [
            "--water-capture", "--edition", "remastered", "--gpu-audit",
            "--experimental-keyboard-hook", "--synthetic-input-script",
            "remastered-boot-menu-to-gameplay-v1",
        ]

        def guest(_executable, output, _game_dir, _profile, _seconds,
                  _quincunx, _audit, **kwargs):
            self.assertTrue(kwargs["water_capture"])
            self.assertEqual(kwargs["water_preset"], "retail")
            (output / "guest.log").write_bytes(
                b"[GPU] ocean draw markers: 4 received, 4 batch scopes, "
                b"0 rejected\n" + self.water_capture_summary() + b"\n"
            )
            (output / "gpu-audit.txt").write_bytes(
                diagnostic.OCEAN_FINGERPRINT_HEADER + b"\n"
                b"  batches 0, hashes 0/32, table-overflow batches 0, "
                b"within-scope mismatches 0\n"
            )
            for image in range(4):
                self.write_capture_bitmap(
                    output / f"water-capture-{image:05d}.bmp"
                )
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
        self.assertIn("four private supported-frame BMPs", output.getvalue())
        self.assertIn("4 private retail frames each at 2x1", output.getvalue())

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

    def test_guided_input_capture_cli_propagates_private_paths(self):
        arguments = self.main_arguments() + [
            "--guided-input-capture", "--edition", "remastered",
            "--experimental-keyboard-hook", "--synthetic-input-script",
            "remastered-boot-menu-to-gameplay-v1",
        ]

        def guest(_executable, output, _game_dir, _profile, _seconds,
                  _quincunx, _audit, **kwargs):
            self.assertTrue(kwargs["guided_input"])
            self.assertEqual(
                kwargs["synthetic_input_script"],
                "remastered-boot-menu-to-gameplay-v1")
            self.assertTrue((output / "guided-pad.txt").is_file())
            (output / "guest.log").write_bytes(b"no marker summary\n")
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
        self.assertIn("guided-pad.txt", output.getvalue())
        self.assertIn("guided-frame-NNNNN.bmp", output.getvalue())

    def test_performance_diagnostic_cli_propagates_options(self):
        warmup = self.root / "warmup.bwmpa"
        warmup.write_bytes(b"private synthetic warmup")
        warmup.chmod(0o600)
        arguments = self.main_arguments() + [
            "--frame-timing", "--pipeline-timing",
            "--pipeline-trace", "--pipeline-warmup", str(warmup),
            "--profile", "--profile-from", "5",
            "--profile-to", "10", "--profile-callers", "--render-scale", "2.25",
            "--frame-cap", "60",
        ]

        def guest(_executable, output, _game_dir, _profile, _seconds,
                  _quincunx, _audit, **kwargs):
            self.assertTrue(kwargs["frame_timing"])
            self.assertTrue(kwargs["pipeline_timing"])
            self.assertTrue(kwargs["pipeline_trace"])
            self.assertEqual(kwargs["pipeline_warmup"], warmup)
            self.assertTrue(kwargs["profile_enabled"])
            self.assertEqual(kwargs["profile_from"], 5.0)
            self.assertEqual(kwargs["profile_to"], 10.0)
            self.assertTrue(kwargs["profile_callers"])
            self.assertEqual(kwargs["render_scale"], "2.25")
            self.assertEqual(kwargs["frame_cap"], 60)
            (output / "guest.log").write_bytes(b"no marker summary\n")
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
        self.assertIn("frame timing: enabled", output.getvalue())
        self.assertIn("pipeline timing: bounded JSONL enabled", output.getvalue())
        self.assertIn("pipeline trace: private bounded artifact enabled",
                      output.getvalue())
        self.assertIn("pipeline warm-up: private artifact enabled",
                      output.getvalue())
        self.assertIn("sampling profile: enabled", output.getvalue())
        self.assertIn("render scale: 2.25", output.getvalue())
        self.assertIn("frame cap: 60", output.getvalue())

    def test_private_save_and_launch_seed_cli(self):
        save_seed = self.root / "save-seed"
        save_file = save_seed / "editions/remastered/save.bin"
        save_file.parent.mkdir(parents=True)
        save_file.write_bytes(b"private save")
        launch_seed = self.root / "launch.bin"
        request = self.launch_request()
        launch_seed.write_bytes(request)
        arguments = self.main_arguments() + [
            "--save-seed", str(save_seed), "--launch-seed", str(launch_seed),
        ]

        def guest(_executable, output, _game_dir, _profile, _seconds,
                  _quincunx, _audit, **kwargs):
            self.assertEqual(kwargs["initial_launch"], request)
            self.assertEqual(
                (output / "saves/editions/remastered/save.bin").read_bytes(),
                b"private save",
            )
            (output / "guest.log").write_bytes(b"no marker summary\n")
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
        self.assertIn("private save seed: 1 files", output.getvalue())
        self.assertIn("validated same-title launch seed", output.getvalue())

    def test_headless_audio_audit_is_explicit_and_propagated(self):
        arguments = self.main_arguments() + ["--headless-audio-audit"]

        def guest(_executable, output, _game_dir, _profile, _seconds, _quincunx, _audit,
                  **kwargs):
            (output / "guest.log").write_bytes(b"no marker summary\n")
            self.assertTrue(kwargs["audio_audit"])
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
        self.assertIn("Diagnostic audio: silent headless audit (not audible)",
                      output.getvalue())

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

    def test_water_semantic_summaries_use_each_boots_final_bounded_counts(self):
        first = self.root / "guest-001.log"
        second = self.root / "guest-002.log"
        first.write_bytes(
            b"[GPU] water pass scopes: lit 2, reflection 3, foam 4\n"
            b"[GPU] water views: begin 2/1/0/0, end 2/1/0/0\n"
            b"[GPU] water pass scopes: lit 5, reflection 7, foam 9\r\n"
            b"[GPU] water views: begin 4/2/0/0, end 4/2/0/0\r\n")
        second.write_bytes(
            b"[GPU] water pass scopes: lit 1, reflection 2, foam 3\n"
            b"[GPU] water views: begin 3/0/0/0, end 3/0/0/0\n")
        self.assertEqual(
            diagnostic.extract_water_semantic_counts([first, second]),
            (((6, 9, 12), (7, 2, 0, 0), (7, 2, 0, 0)), (3, 3)))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            counts = diagnostic.report_water_semantic_counts([first, second])
        self.assertEqual(counts, ((6, 9, 12), (7, 2, 0, 0), (7, 2, 0, 0)))
        self.assertIn("Water passes: 6 lit, 9 reflection, 12 foam scopes",
                      output.getvalue())
        self.assertIn("Water views: begin 7/2/0/0, end 7/2/0/0",
                      output.getvalue())

    def test_water_semantic_summary_validation(self):
        log = self.root / "guest.log"
        malformed = (
            b"[GPU] water pass scopes: lit 1, missing fields\n",
            b"[GPU] water pass scopes: lit 18446744073709551616, "
            b"reflection 1, foam 1\n",
            b"[GPU] water pass scopes: lit 2, reflection 2, foam 2\n"
            b"[GPU] water pass scopes: lit 1, reflection 2, foam 2\n",
            b"[GPU] water views: begin 1/0/0/0, missing fields\n",
            b"[GPU] water views: begin 1/0/0/0, end 2/0/0/0\n",
            b"[GPU] water views: begin 2/0/0/0, end 1/0/0/0\n"
            b"[GPU] water views: begin 1/0/0/0, end 1/0/0/0\n",
        )
        for contents in malformed:
            with self.subTest(contents=contents):
                log.write_bytes(contents)
                with self.assertRaisesRegex(
                        RuntimeError, "malformed|out-of-range|non-monotonic|inconsistent"):
                    diagnostic.extract_water_semantic_counts([log])

    def test_missing_water_semantic_summaries_report_zero_coverage(self):
        log = self.root / "guest.log"
        log.write_bytes(b"unrelated guest output\n")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            counts = diagnostic.report_water_semantic_counts([log])
        self.assertEqual(counts, ((0, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0)))
        self.assertIn("zero observed semantic coverage", output.getvalue())
        self.assertIn("zero observed view coverage", output.getvalue())

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
