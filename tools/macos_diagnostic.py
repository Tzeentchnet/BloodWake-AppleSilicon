"""Run one native guest boot in a private, bounded macOS sandbox."""

import argparse
from dataclasses import dataclass
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import selectors
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
from typing import Optional


ROOT = Path(__file__).resolve().parents[1]
TOOLKIT_COMMIT = "f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b"
MAX_LOG_BYTES = 32 * 1024 * 1024
MAX_RELAUNCH_BYTES = 16 + 260 + 3072
MAX_RELAUNCH_BOOTS = 16
MAX_SAVE_SEED_FILES = 4096
MAX_SAVE_SEED_BYTES = 64 * 1024 * 1024
SANDBOX = "/usr/bin/sandbox-exec"
EDITIONS = ("original", "remastered")
SYNTHETIC_INPUT_SCRIPTS = (
    "remastered-boot-menu-to-gameplay-v1",
    "remastered-boot-menu-to-gameplay-2p-v1",
)
OCEAN_MARKER_PREFIX = b"[GPU] ocean draw markers:"
OCEAN_MARKER_PATTERN = re.compile(
    rb"\[GPU\] ocean draw markers: ([0-9]{1,20}) received, "
    rb"([0-9]{1,20}) batch scopes, ([0-9]{1,20}) rejected"
)
WATER_PASS_PREFIX = b"[GPU] water pass scopes:"
WATER_PASS_PATTERN = re.compile(
    rb"\[GPU\] water pass scopes: lit ([0-9]{1,20}), "
    rb"reflection ([0-9]{1,20}), foam ([0-9]{1,20})"
)
WATER_VIEW_PREFIX = b"[GPU] water views:"
WATER_VIEW_PATTERN = re.compile(
    rb"\[GPU\] water views: begin "
    rb"([0-9]{1,20})/([0-9]{1,20})/([0-9]{1,20})/([0-9]{1,20}), end "
    rb"([0-9]{1,20})/([0-9]{1,20})/([0-9]{1,20})/([0-9]{1,20})"
)
MAX_OCEAN_MARKER_COUNT = (1 << 64) - 1
MAX_OCEAN_MARKER_SUMMARIES = 4096
WATER_BENCHMARK_SAMPLES_PER_PRESET = 100
OCEAN_FINGERPRINT_HEADER = (
    b"== Marked ocean batch fingerprints (v1 FNV-1a-64; stable metadata only)"
)
OCEAN_FINGERPRINT_PATTERN = re.compile(
    rb"  batches ([0-9]{1,20}), hashes ([0-9]{1,2})/32, "
    rb"table-overflow batches ([0-9]{1,20}), "
    rb"within-scope mismatches ([0-9]{1,20})"
)
OCEAN_FINGERPRINT_HASH_PATTERN = re.compile(
    rb"  ([0-9A-F]{16}) +([0-9]{1,20}) batches"
)
REALISTIC_WATER_ACTIVE = (
    b"[GPU-METAL] optional realistic water shader active for explicitly marked ocean passes"
)
REALISTIC_WATER_FALLBACK_PREFIX = (
    b"[GPU-METAL] realistic water encountered an unsupported marked pass "
)
WATER_BENCHMARK_PREFIX = b"[GPU-METAL] water benchmark v1 "
WATER_BENCHMARK_PATTERN = re.compile(
    rb"(?m)^\[GPU-METAL\] water benchmark v1 "
    rb"retail_samples=([0-9]{1,5}) retail_mean_ms=([0-9]+\.[0-9]{6}) "
    rb"retail_median_ms=([0-9]+\.[0-9]{6}) retail_p95_ms=([0-9]+\.[0-9]{6}) "
    rb"realistic_samples=([0-9]{1,5}) realistic_mean_ms=([0-9]+\.[0-9]{6}) "
    rb"realistic_median_ms=([0-9]+\.[0-9]{6}) realistic_p95_ms=([0-9]+\.[0-9]{6}) "
    rb"mean_delta_percent=([+-][0-9]+\.[0-9]{6}) "
    rb"median_delta_percent=([+-][0-9]+\.[0-9]{6}) "
    rb"p95_delta_percent=([+-][0-9]+\.[0-9]{6})\r?$"
)
WATER_CAPTURE_PREFIX = b"[GPU-METAL] water capture v1 "
WATER_CAPTURE_PATTERN = re.compile(
    rb"(?m)^\[GPU-METAL\] water capture v1 "
    rb"preset=(retail|realistic) images=4 "
    rb"supported_offsets=0,60,120,180 "
    rb"width=([0-9]{1,5}) height=([0-9]{1,5})\r?$"
)
WATER_CAPTURE_IMAGES = 4


@dataclass(frozen=True)
class RelaunchRunResult:
    guest_status: Optional[int]
    completed_boots: int
    boot_limit_reached: bool

    @classmethod
    def guest_exit(cls, status, completed_boots):
        return cls(status, completed_boots, False)

    @classmethod
    def boot_limit(cls, completed_boots):
        return cls(None, completed_boots, True)


def digest_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def snapshot_tree(root):
    """Include modes, directories and symlink targets without following links."""
    root = Path(root).resolve(strict=True)
    records = {}
    def walk_error(error):
        raise error
    for parent, directories, files in os.walk(root, followlinks=False, onerror=walk_error):
        for name in sorted(directories + files):
            path = Path(parent) / name
            info = path.lstat()
            relative = path.relative_to(root).as_posix()
            mode = stat.S_IMODE(info.st_mode)
            if stat.S_ISREG(info.st_mode):
                records[relative] = ("file", mode, info.st_size, digest_file(path))
            elif stat.S_ISDIR(info.st_mode):
                records[relative] = ("directory", mode)
            elif stat.S_ISLNK(info.st_mode):
                target = path.resolve(strict=True)
                if target != root and root not in target.parents:
                    raise RuntimeError("prepared tree contains a symlink outside the tree")
                records[relative] = ("symlink", mode, os.readlink(path))
            else:
                raise RuntimeError("prepared tree contains an unsupported filesystem entry")
    root_info = root.stat()
    records["."] = ("directory", stat.S_IMODE(root_info.st_mode))
    return records


def toolkit_revision(toolkit):
    def git(*args):
        result = subprocess.run(
            ["git", "-C", str(toolkit), *args],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True,
        )
        return result.stdout.strip()

    revision = git("rev-parse", "HEAD")
    if revision != TOOLKIT_COMMIT or git("status", "--porcelain", "--untracked-files=all"):
        raise RuntimeError("toolkit must be a clean checkout at the pinned commit")
    return revision


def verify_prepared(prepared, game_dir):
    spec = importlib.util.spec_from_file_location("build_from_disc", ROOT / "tools/build_from_disc.py")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    try:
        builder.verify_gen(str(ROOT), str(prepared / "work/gen"), lambda _step, _pct, msg: print(msg))
        retail = builder.Retail(builder.load_json(str(ROOT / "fingerprints/retail.json"), "retail fingerprint"))
    except builder.Fail as error:
        raise RuntimeError("prepared verification failed: " + str(error)) from error
    files = {
        rel: (os.path.getsize(path), builder.sha256_file(path))
        for rel, path in builder.folder_files(str(game_dir))
    }
    ok, message, details = retail.compare(files)
    if not ok or details["extra"]:
        raise RuntimeError("prepared disc does not exactly match the retail fingerprint")
    print(message)


def overlaps(a, b):
    return a == b or a in b.parents or b in a.parents


def create_output(path, prepared, toolkit):
    path = Path(path).expanduser()
    if not path.name or path.name in (".", ".."):
        raise ValueError("output must name a new directory")
    parent = path.parent.resolve(strict=True)
    output = parent / path.name
    if any(overlaps(output, protected) for protected in (ROOT, prepared, toolkit)):
        raise ValueError("output must be outside the repository, prepared tree and toolkit")
    output.mkdir(mode=0o700)
    return output


def copy_save_seed(source, destination):
    source = source.resolve()
    if not source.is_dir():
        raise ValueError("save seed must be a directory")
    file_count = 0
    byte_count = 0
    for root, directories, files in os.walk(source):
        root_path = Path(root)
        if root_path.is_symlink():
            raise ValueError("save seed must not contain symlinks")
        for name in directories:
            path = root_path / name
            if path.is_symlink():
                raise ValueError("save seed must not contain symlinks")
            relative = path.relative_to(source)
            (destination / relative).mkdir(mode=0o700, exist_ok=True)
        for name in files:
            path = root_path / name
            if path.is_symlink() or not path.is_file():
                raise ValueError("save seed must contain only regular files")
            size = path.stat().st_size
            file_count += 1
            byte_count += size
            if (file_count > MAX_SAVE_SEED_FILES or
                    byte_count > MAX_SAVE_SEED_BYTES):
                raise ValueError("save seed exceeds the private diagnostic limit")
            target = destination / path.relative_to(source)
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            target.chmod(0o600)
    return file_count, byte_count


def sandbox_profile(output):
    output_text = str(output)
    if any(ord(char) < 32 for char in output_text):
        raise ValueError("output path contains a control character")
    quoted = json.dumps(output_text, ensure_ascii=False)
    return (
        "(version 1) (allow default) (deny network*) (deny process-fork) "
        "(deny file-write*) (allow file-write* (subpath " + quoted + "))"
    )


PROBE = r"""
import errno, os, socket, sys
output, protected, outside = sys.argv[1:]
allowed = os.path.join(output, "sandbox-write-probe")
with open(allowed, "x") as stream:
    stream.write("probe")
os.unlink(allowed)
for path in (protected, outside):
    try:
        fd = os.open(path, os.O_WRONLY)
    except OSError as error:
        if error.errno not in (errno.EPERM, errno.EACCES):
            raise
    else:
        os.close(fd)
        raise RuntimeError("write outside the diagnostic directory was not denied")
for create, name in ((os.symlink, "sandbox-symlink-probe"),
                     (os.link, "sandbox-hardlink-probe")):
    alias = os.path.join(output, name)
    try:
        create(outside, alias)
    except OSError as error:
        if error.errno not in (errno.EPERM, errno.EACCES):
            raise
        continue
    try:
        try:
            fd = os.open(alias, os.O_WRONLY)
        except OSError as error:
            if error.errno not in (errno.EPERM, errno.EACCES):
                raise
        else:
            os.close(fd)
            raise RuntimeError("write through an external link was not denied")
    finally:
        os.unlink(alias)
for family, address in ((socket.AF_INET, ("127.0.0.1", 9)),
                        (socket.AF_INET6, ("::1", 9))):
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        try:
            with socket.socket(family, kind) as connection:
                connection.connect(address)
        except OSError as error:
            if error.errno not in (errno.EPERM, errno.EACCES):
                raise
        else:
            raise RuntimeError("network connection was not denied")
try:
    pid = os.fork()
except OSError as error:
    if error.errno not in (errno.EPERM, errno.EACCES):
        raise
else:
    if pid == 0:
        os._exit(0)
    os.waitpid(pid, 0)
    raise RuntimeError("process fork was not denied")
try:
    pid = os.posix_spawn("/usr/bin/true", ["/usr/bin/true"], {})
except OSError as error:
    if error.errno not in (errno.EPERM, errno.EACCES):
        raise
else:
    os.waitpid(pid, 0)
    raise RuntimeError("process spawn was not denied")
"""


def verify_sandbox(profile, output, protected):
    with tempfile.NamedTemporaryFile(prefix=".bw-sandbox-probe-", dir=output.parent) as outside:
        result = subprocess.run(
            [SANDBOX, "-p", profile, sys.executable, "-c", PROBE,
             str(output), str(protected), outside.name],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=15,
        )
    if result.returncode:
        raise RuntimeError("sandbox preflight failed: " + result.stderr.strip())
    print("Sandbox preflight: private writes allowed; prepared-tree writes, "
          "IPv4/IPv6 TCP/UDP, fork and posix_spawn denied")


def diagnostic_env(output, game_dir, seconds, quincunx, audit, input_fd, output_fd,
                   audit_name="gpu-audit.txt", keyboard_hook=False, synthetic_input=False,
                   edition="original", disable_audio=False, synthetic_input_script=None,
                   audio_audit=False, water_preset="retail",
                   water_benchmark=False, water_capture=False,
                   capture_stem="water-capture", guided_input=False,
                   frame_timing=False, pipeline_timing=False,
                   profile_name=None, profile_from=None,
                   profile_to=None, profile_callers=False, render_scale=None,
                   frame_cap=None, pipeline_trace_name=None,
                   pipeline_warmup=None):
    if (synthetic_input or synthetic_input_script) and not keyboard_hook:
        raise ValueError("synthetic input requires the experimental keyboard hook")
    if synthetic_input and synthetic_input_script:
        raise ValueError("legacy synthetic input and a synthetic input script are mutually exclusive")
    if guided_input and (keyboard_hook or synthetic_input or synthetic_input_script):
        raise ValueError("guided input cannot use the experimental keyboard input path")
    if synthetic_input_script not in (None, *SYNTHETIC_INPUT_SCRIPTS):
        raise ValueError("unknown synthetic input script")
    if edition not in EDITIONS:
        raise ValueError("edition must be original or remastered")
    if water_preset not in ("retail", "realistic"):
        raise ValueError("water preset must be retail or realistic")
    if water_preset == "realistic" and edition != "remastered":
        raise ValueError("realistic water requires the remastered edition")
    if water_benchmark and edition != "remastered":
        raise ValueError("water benchmark requires the remastered edition")
    if water_benchmark and water_preset != "retail":
        raise ValueError("water benchmark requires the retail water preset")
    if water_benchmark and not audit:
        raise ValueError("water benchmark requires GPU audit")
    if water_capture and edition != "remastered":
        raise ValueError("water capture requires the remastered edition")
    if water_capture and not audit:
        raise ValueError("water capture requires GPU audit")
    if water_capture and water_benchmark:
        raise ValueError("water capture and benchmark are mutually exclusive")
    if guided_input and water_capture:
        raise ValueError("guided input and water capture are mutually exclusive")
    if (water_capture and
            synthetic_input_script != SYNTHETIC_INPUT_SCRIPTS[0]):
        raise ValueError("water capture requires the deterministic synthetic input script")
    if water_capture and (
            not capture_stem or "/" in capture_stem or "\\" in capture_stem):
        raise ValueError("water capture stem is invalid")
    if synthetic_input_script and edition != "remastered":
        raise ValueError("the synthetic input script requires the remastered edition")
    if disable_audio and audio_audit:
        raise ValueError("headless audio audit requires audio to remain enabled")
    if profile_name and (
            Path(profile_name).name != profile_name or
            not profile_name.endswith(".txt")):
        raise ValueError("profile output name is invalid")
    if profile_from is not None and profile_from < 0:
        raise ValueError("profile start must not be negative")
    if profile_to is not None and profile_to <= 0:
        raise ValueError("profile end must be positive")
    if (profile_from is not None and profile_to is not None and
            profile_to <= profile_from):
        raise ValueError("profile end must be after its start")
    if (profile_from is not None or profile_to is not None) and not profile_name:
        raise ValueError("profile window requires profile output")
    if profile_callers and not profile_name:
        raise ValueError("profile caller sampling requires profile output")
    if render_scale is not None:
        if render_scale != "auto":
            try:
                scale = float(render_scale)
            except ValueError as error:
                raise ValueError("render scale must be auto or 0.5 through 10") from error
            if not math.isfinite(scale) or scale < 0.5 or scale > 10:
                raise ValueError("render scale must be auto or 0.5 through 10")
    if frame_cap not in (None, 30, 60, 120):
        raise ValueError("frame cap must be 30, 60, or 120")
    if pipeline_trace_name and (
            Path(pipeline_trace_name).name != pipeline_trace_name or
            not pipeline_trace_name.endswith(".bwmpa")):
        raise ValueError("pipeline trace output name is invalid")
    if pipeline_warmup is not None:
        warmup = Path(pipeline_warmup)
        if not warmup.is_absolute() or warmup.suffix != ".bwmpa":
            raise ValueError("pipeline warm-up input must be an absolute .bwmpa path")
    env = {
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME": str(output / "home"),
        "TMPDIR": str(output / "tmp") + "/",
        "XDG_CONFIG_HOME": str(output / "config"),
        "XDG_DATA_HOME": str(output / "data"),
        "RECOMP_SAVE_DIR": str(output / "saves"),
        "BW_SETTINGS": "none",
        "BW_GAME_DIR": str(game_dir),
        "BW_EDITION": edition,
        "BW_RUN_SECONDS": str(seconds),
        "BW_NATIVE_DIAGNOSTIC": "1",
        "BW_NATIVE_CHILD": "1",
        "BW_NATIVE_LAUNCH_IN_FD": str(input_fd),
        "BW_NATIVE_LAUNCH_OUT_FD": str(output_fd),
    }
    if quincunx:
        env["BW_METAL_QUINCUNX_APPROX"] = "1"
    if keyboard_hook:
        env["BW_MACOS_KEYBOARD_HOOK"] = "1"
    if synthetic_input:
        env["BW_SYNTHETIC_INPUT_DIAGNOSTIC"] = "1"
    if synthetic_input_script:
        env["BW_SYNTHETIC_INPUT_SCRIPT"] = synthetic_input_script
    if disable_audio:
        env["BW_MACOS_AUDIO"] = "0"
    if audio_audit:
        env["BW_HEADLESS_AUDIO_AUDIT"] = "1"
    if water_preset == "realistic":
        env["BW_METAL_WATER"] = "realistic"
    if water_benchmark:
        env["BW_METAL_WATER_BENCHMARK"] = "1"
    if water_capture:
        prefix = output / f"{capture_stem}-"
        trigger = output / f".{capture_stem}.trigger"
        env["RECOMP_FB_DUMP"] = str(prefix)
        env["RECOMP_FB_DUMP_FLIPS"] = str(trigger)
        env["BW_METAL_WATER_CAPTURE_TRIGGER"] = str(trigger)
    if guided_input:
        env["BW_MACOS_KEYBOARD_HOOK"] = "1"
        env["BW_MACOS_GUIDED_INPUT"] = str(output / "guided-pad.txt")
        env["RECOMP_FB_DUMP"] = str(output / "guided-frame-")
        env["RECOMP_PB_REPORT_MS"] = "2000"
    if frame_timing:
        env["BW_METAL_FRAME_TIMING"] = "1"
    if pipeline_timing:
        env["BW_METAL_PIPELINE_TIMING"] = "1"
    if pipeline_trace_name:
        env["BW_METAL_PIPELINE_TRACE_OUTPUT"] = str(output / pipeline_trace_name)
    if pipeline_warmup is not None:
        env["BW_METAL_PIPELINE_WARMUP"] = str(pipeline_warmup)
    if profile_name:
        env["BW_PROFILE"] = str(output / profile_name)
        if profile_from is not None:
            env["BW_PROFILE_FROM"] = str(profile_from)
        if profile_to is not None:
            env["BW_PROFILE_TO"] = str(profile_to)
        if profile_callers:
            env["BW_PROFILE_CALLERS"] = "1"
    if render_scale is not None:
        env["BW_METAL_RENDER_SCALE"] = render_scale
    if frame_cap is not None:
        env["BW_HIFPS"] = str(frame_cap)
    if audit:
        env["BW_GPU_AUDIT"] = "1"
        env["BW_GPU_AUDIT_EVERY"] = "5"
        env["BW_GPU_AUDIT_FILE"] = str(output / audit_name)
    return env


def capture_guest_output(child, output_read, log, timeout, log_limit=None):
    if log_limit is None:
        log_limit = MAX_LOG_BYTES
    deadline = time.monotonic() + timeout
    log_size = 0
    relaunch = bytearray()
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(child.stdout, selectors.EVENT_READ, "log")
            selector.register(output_read, selectors.EVENT_READ, "relaunch")
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError("guest exceeded the external deadline and was terminated")
                for key, _ in selector.select(remaining):
                    data = os.read(key.fd, 65536)
                    if not data:
                        selector.unregister(key.fileobj)
                    elif key.data == "log":
                        allowed = min(len(data), log_limit - log_size)
                        log.write(data[:allowed])
                        log_size += allowed
                        if allowed != len(data):
                            raise RuntimeError("guest log exceeded its size limit and was terminated")
                    else:
                        relaunch.extend(data)
                        if len(relaunch) > MAX_RELAUNCH_BYTES:
                            raise RuntimeError("guest returned an oversized relaunch request")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("guest exceeded the external deadline and was terminated")
            try:
                result = child.wait(timeout=remaining)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError("guest exceeded the external deadline and was terminated") from error
        return result, bytes(relaunch)
    finally:
        try:
            if child.poll() is None:
                child.kill()
                child.wait()
        finally:
            child.stdout.close()


def validate_relaunch(data):
    if len(data) < 16 or data[:4] != b"BWRL":
        raise RuntimeError("guest returned an invalid relaunch request")
    version, path_length = struct.unpack_from("<II", data, 4)
    if version != 1 or path_length > 260 or len(data) < 16 + path_length:
        raise RuntimeError("guest returned an invalid relaunch request")
    path = data[12:12 + path_length]
    data_length = struct.unpack_from("<I", data, 12 + path_length)[0]
    if b"\0" in path or data_length not in (0, 3072) or len(data) != 16 + path_length + data_length:
        raise RuntimeError("guest returned an invalid relaunch request")
    path = path.lower()
    if path.startswith((b"d:\\", b"d:/")):
        path = path[3:]
    elif path.startswith(b"\\device\\cdrom0;"):
        path = path[15:]
    if path != b"default.xbe":
        raise RuntimeError("guest relaunch request targets another title")


def run_guest_once(executable, output, game_dir, profile, seconds, quincunx, audit,
                   launch=b"", log_name="guest.log", audit_name="gpu-audit.txt",
                   timeout=None, log_limit=None, keyboard_hook=False, synthetic_input=False,
                   edition="original", disable_audio=False, synthetic_input_script=None,
                   audio_audit=False, water_preset="retail",
                   water_benchmark=False, water_capture=False,
                   capture_stem="water-capture", guided_input=False,
                   frame_timing=False, pipeline_timing=False,
                   profile_name=None, profile_from=None,
                   profile_to=None, profile_callers=False, render_scale=None,
                   frame_cap=None, pipeline_trace_name=None,
                   pipeline_warmup=None):
    input_read = input_write = output_read = output_write = -1
    child = None
    try:
        input_read, input_write = os.pipe()
        output_read, output_write = os.pipe()
        if launch:
            validate_relaunch(launch)
            os.set_blocking(input_write, False)
            remaining = memoryview(launch)
            while remaining:
                try:
                    written = os.write(input_write, remaining)
                except BlockingIOError as error:
                    raise RuntimeError("relaunch request does not fit in the input pipe") from error
                if written <= 0:
                    raise RuntimeError("cannot write the relaunch request")
                remaining = remaining[written:]
        os.close(input_write)
        input_write = -1
        env = diagnostic_env(output, game_dir, seconds, quincunx, audit, input_read, output_write,
                             audit_name, keyboard_hook, synthetic_input, edition, disable_audio,
                             synthetic_input_script, audio_audit, water_preset,
                             water_benchmark, water_capture, capture_stem, guided_input,
                             frame_timing, pipeline_timing, profile_name,
                             profile_from, profile_to,
                             profile_callers, render_scale, frame_cap,
                             pipeline_trace_name, pipeline_warmup)
        with open(output / log_name, "xb") as log:
            child = subprocess.Popen(
                [SANDBOX, "-p", profile, str(executable), "--bw-native-child"],
                cwd=output, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                pass_fds=(input_read, output_write),
            )
            os.close(input_read)
            input_read = -1
            os.close(output_write)
            output_write = -1
            result, relaunch = capture_guest_output(child, output_read, log,
                                                    seconds + 15 if timeout is None else timeout,
                                                    log_limit)
            log_size = log.tell()
        return result, relaunch, log_size
    finally:
        try:
            if child is not None and child.poll() is None:
                child.kill()
                child.wait()
        finally:
            if child is not None and child.stdout is not None:
                child.stdout.close()
            for fd in (input_read, input_write, output_read, output_write):
                if fd >= 0:
                    os.close(fd)


def run_guest(executable, output, game_dir, profile, seconds, quincunx, audit,
              keyboard_hook=False, synthetic_input=False, edition="original",
              disable_audio=False, synthetic_input_script=None, audio_audit=False,
              water_preset="retail", water_benchmark=False,
              water_capture=False, guided_input=False, frame_timing=False,
              pipeline_timing=False,
              profile_enabled=False, profile_from=None, profile_to=None,
              profile_callers=False, render_scale=None, frame_cap=None,
              initial_launch=b"", pipeline_trace=False,
              pipeline_warmup=None):
    result, relaunch, _ = run_guest_once(executable, output, game_dir, profile,
                                         seconds, quincunx, audit, launch=initial_launch,
                                         keyboard_hook=keyboard_hook,
                                         synthetic_input=synthetic_input, edition=edition,
                                         disable_audio=disable_audio,
                                         synthetic_input_script=synthetic_input_script,
                                         audio_audit=audio_audit,
                                         water_preset=water_preset,
                                         water_benchmark=water_benchmark,
                                         water_capture=water_capture,
                                         guided_input=guided_input,
                                         frame_timing=frame_timing,
                                         pipeline_timing=pipeline_timing,
                                         profile_name="profile.txt" if profile_enabled else None,
                                         profile_from=profile_from, profile_to=profile_to,
                                         profile_callers=profile_callers,
                                         render_scale=render_scale, frame_cap=frame_cap,
                                         pipeline_trace_name=(
                                             "pipeline-trace.bwmpa"
                                             if pipeline_trace else None),
                                         pipeline_warmup=pipeline_warmup)
    if relaunch:
        (output / "relaunch.bin").write_bytes(relaunch)
        print("Guest requested a relaunch; request saved privately, not executed")
    return result


def run_guest_relaunches(executable, output, game_dir, profile, seconds, quincunx,
                         audit, max_boots, total_seconds, keyboard_hook=False,
                         synthetic_input=False, edition="original", disable_audio=False,
                         synthetic_input_script=None, audio_audit=False,
                         water_preset="retail", water_benchmark=False,
                         water_capture=False, guided_input=False, frame_timing=False,
                         pipeline_timing=False,
                         profile_enabled=False, profile_from=None, profile_to=None,
                         profile_callers=False, render_scale=None, frame_cap=None,
                         initial_launch=b"", pipeline_trace=False,
                         pipeline_warmup=None):
    deadline = time.monotonic() + total_seconds
    log_remaining = MAX_LOG_BYTES
    launch = initial_launch
    completed_boots = 0
    for boot in range(1, max_boots + 1):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("guest exceeded the total diagnostic deadline")
        result, relaunch, log_size = run_guest_once(
            executable, output, game_dir, profile, seconds, quincunx, audit,
            launch=launch, log_name=f"guest-{boot:03d}.log",
            audit_name=f"gpu-audit-{boot:03d}.txt",
            timeout=min(seconds + 15, remaining), log_limit=log_remaining,
            keyboard_hook=keyboard_hook, synthetic_input=synthetic_input, edition=edition,
            disable_audio=disable_audio, synthetic_input_script=synthetic_input_script,
            audio_audit=audio_audit, water_preset=water_preset,
            water_benchmark=water_benchmark, water_capture=water_capture,
            capture_stem=f"water-capture-{boot:03d}",
            guided_input=guided_input, frame_timing=frame_timing,
            pipeline_timing=pipeline_timing,
            profile_name=f"profile-{boot:03d}.txt" if profile_enabled else None,
            profile_from=profile_from, profile_to=profile_to,
            profile_callers=profile_callers,
            render_scale=render_scale, frame_cap=frame_cap,
            pipeline_trace_name=(
                f"pipeline-trace-{boot:03d}.bwmpa"
                if pipeline_trace else None),
            pipeline_warmup=pipeline_warmup)
        completed_boots = boot
        log_remaining -= log_size
        if relaunch:
            with open(output / f"relaunch-{boot:03d}.bin", "xb") as record:
                record.write(relaunch)
        if result != 0 or not relaunch:
            return RelaunchRunResult.guest_exit(result, completed_boots)
        validate_relaunch(relaunch)
        if boot == max_boots:
            return RelaunchRunResult.boot_limit(completed_boots)
        launch = relaunch
        print(f"Following validated guest relaunch for boot {boot + 1}")
    raise RuntimeError("relaunch loop ended without a result")


def guest_log_paths(output, follow_relaunches, completed_boots, max_boots):
    if not follow_relaunches:
        path = output / "guest.log"
        if completed_boots != 1 or not path.is_file():
            raise RuntimeError("guest log is missing")
        return [path]

    paths = sorted(output.glob("guest-*.log"))
    expected = [output / f"guest-{boot:03d}.log"
                for boot in range(1, completed_boots + 1)]
    if not 1 <= completed_boots <= max_boots or paths != expected:
        raise RuntimeError("numbered guest logs are missing or out of sequence")
    return paths


def extract_ocean_marker_counts(log_paths):
    totals = [0, 0, 0]
    summary_count = 0
    for path in log_paths:
        previous = None
        with open(path, "rb") as stream:
            for line in stream:
                if OCEAN_MARKER_PREFIX not in line:
                    continue
                summary_count += 1
                if summary_count > MAX_OCEAN_MARKER_SUMMARIES:
                    raise RuntimeError("guest logs contain too many ocean marker summaries")
                match = OCEAN_MARKER_PATTERN.fullmatch(line.rstrip(b"\r\n"))
                if match is None:
                    raise RuntimeError("guest log contains a malformed ocean marker summary")
                counts = tuple(int(value) for value in match.groups())
                if any(value > MAX_OCEAN_MARKER_COUNT for value in counts):
                    raise RuntimeError("guest log contains an out-of-range ocean marker count")
                if previous is not None and any(value < old for value, old in zip(counts, previous)):
                    raise RuntimeError("guest log contains non-monotonic ocean marker counts")
                previous = counts
        if previous is not None:
            for index, value in enumerate(previous):
                if totals[index] > MAX_OCEAN_MARKER_COUNT - value:
                    raise RuntimeError("aggregate ocean marker count is out of range")
                totals[index] += value
    return tuple(totals), summary_count


def report_ocean_marker_counts(log_paths):
    counts, summary_count = extract_ocean_marker_counts(log_paths)
    received, batches, rejected = counts
    suffix = ""
    if not summary_count:
        suffix = " (no marker summary; zero observed ocean coverage)"
    elif not batches:
        suffix = " (zero observed ocean coverage)"
    print(f"Ocean markers: {received} received, {batches} batch scopes, "
          f"{rejected} rejected{suffix}")
    return counts


def extract_water_semantic_counts(log_paths):
    pass_totals = [0, 0, 0]
    begin_totals = [0, 0, 0, 0]
    end_totals = [0, 0, 0, 0]
    pass_summary_count = view_summary_count = 0

    for path in log_paths:
        previous_passes = previous_views = None
        with open(path, "rb") as stream:
            for line in stream:
                line = line.rstrip(b"\r\n")
                if WATER_PASS_PREFIX in line:
                    pass_summary_count += 1
                    if pass_summary_count > MAX_OCEAN_MARKER_SUMMARIES:
                        raise RuntimeError("guest logs contain too many water pass summaries")
                    match = WATER_PASS_PATTERN.fullmatch(line)
                    if match is None:
                        raise RuntimeError("guest log contains a malformed water pass summary")
                    counts = tuple(int(value) for value in match.groups())
                    if any(value > MAX_OCEAN_MARKER_COUNT for value in counts):
                        raise RuntimeError("guest log contains an out-of-range water pass count")
                    if (previous_passes is not None and
                            any(value < old for value, old in zip(counts, previous_passes))):
                        raise RuntimeError("guest log contains non-monotonic water pass counts")
                    previous_passes = counts
                elif WATER_VIEW_PREFIX in line:
                    view_summary_count += 1
                    if view_summary_count > MAX_OCEAN_MARKER_SUMMARIES:
                        raise RuntimeError("guest logs contain too many water view summaries")
                    match = WATER_VIEW_PATTERN.fullmatch(line)
                    if match is None:
                        raise RuntimeError("guest log contains a malformed water view summary")
                    counts = tuple(int(value) for value in match.groups())
                    if any(value > MAX_OCEAN_MARKER_COUNT for value in counts):
                        raise RuntimeError("guest log contains an out-of-range water view count")
                    if (previous_views is not None and
                            any(value < old for value, old in zip(counts, previous_views))):
                        raise RuntimeError("guest log contains non-monotonic water view counts")
                    if any(end > begin for begin, end in zip(counts[:4], counts[4:])):
                        raise RuntimeError("guest log contains inconsistent water view counts")
                    previous_views = counts

        if previous_passes is not None:
            for index, value in enumerate(previous_passes):
                if pass_totals[index] > MAX_OCEAN_MARKER_COUNT - value:
                    raise RuntimeError("aggregate water pass count is out of range")
                pass_totals[index] += value
        if previous_views is not None:
            for totals, values in ((begin_totals, previous_views[:4]),
                                   (end_totals, previous_views[4:])):
                for index, value in enumerate(values):
                    if totals[index] > MAX_OCEAN_MARKER_COUNT - value:
                        raise RuntimeError("aggregate water view count is out of range")
                    totals[index] += value

    return ((tuple(pass_totals), tuple(begin_totals), tuple(end_totals)),
            (pass_summary_count, view_summary_count))


def report_water_semantic_counts(log_paths):
    counts, summary_counts = extract_water_semantic_counts(log_paths)
    passes, begins, ends = counts
    pass_summaries, view_summaries = summary_counts
    pass_suffix = "" if pass_summaries else " (no pass summary; zero observed semantic coverage)"
    view_suffix = "" if view_summaries else " (no view summary; zero observed view coverage)"
    print(f"Water passes: {passes[0]} lit, {passes[1]} reflection, "
          f"{passes[2]} foam scopes{pass_suffix}")
    print("Water views: "
          f"begin {begins[0]}/{begins[1]}/{begins[2]}/{begins[3]}, "
          f"end {ends[0]}/{ends[1]}/{ends[2]}/{ends[3]}{view_suffix}")
    return counts


def extract_realistic_water_coverage(log_paths):
    active_boots = fallback_boots = 0
    for path in log_paths:
        active = fallback = False
        with open(path, "rb") as stream:
            for line in stream:
                line = line.rstrip(b"\r\n")
                if line == REALISTIC_WATER_ACTIVE:
                    if active:
                        raise RuntimeError("guest log contains duplicate realistic-water activation")
                    active = True
                elif line.startswith(REALISTIC_WATER_FALLBACK_PREFIX):
                    if fallback:
                        raise RuntimeError("guest log contains duplicate realistic-water fallback")
                    fallback = True
        active_boots += active
        fallback_boots += fallback
    return active_boots, fallback_boots


def report_realistic_water_coverage(log_paths, water_preset,
                                    water_benchmark=False):
    active_boots, fallback_boots = extract_realistic_water_coverage(log_paths)
    if water_benchmark:
        if not active_boots:
            raise RuntimeError(
                "water benchmark did not activate realistic water in any "
                "guest boot"
            )
        print(
            "Realistic water benchmark: shader path active in "
            f"{active_boots} boots; unsupported-pass fallback observed in "
            f"{fallback_boots} boots"
        )
    elif water_preset == "realistic":
        if not active_boots:
            raise RuntimeError("realistic water did not activate in any guest boot")
        print(f"Realistic water: active in {active_boots} boots; unsupported-pass "
              f"fallback observed in {fallback_boots} boots")
    elif active_boots or fallback_boots:
        raise RuntimeError("realistic-water activity was observed under the retail preset")
    return active_boots, fallback_boots


def read_water_benchmarks(log_paths):
    benchmarks = []
    for path in log_paths:
        data = path.read_bytes()
        matches = list(WATER_BENCHMARK_PATTERN.finditer(data))
        prefixes = data.count(WATER_BENCHMARK_PREFIX)
        if prefixes != len(matches):
            raise RuntimeError(
                f"malformed water benchmark summary in {path.name}"
            )
        if len(matches) > 1:
            raise RuntimeError(
                f"duplicate water benchmark summaries in {path.name}"
            )
        if not matches:
            continue

        groups = matches[0].groups()
        retail_samples = int(groups[0])
        realistic_samples = int(groups[4])
        retail = tuple(float(value) for value in groups[1:4])
        realistic = tuple(float(value) for value in groups[5:8])
        reported_delta = tuple(float(value) for value in groups[8:11])
        values = retail + realistic + reported_delta
        if (retail_samples != realistic_samples or
                retail_samples != WATER_BENCHMARK_SAMPLES_PER_PRESET):
            raise RuntimeError(
                f"inconsistent water benchmark sample counts in {path.name}"
            )
        if not all(math.isfinite(value) for value in values):
            raise RuntimeError(
                f"non-finite water benchmark value in {path.name}"
            )
        if any(value <= 0.0 for value in retail + realistic):
            raise RuntimeError(
                f"non-positive water benchmark timing in {path.name}"
            )

        calculated_delta = tuple(
            (realistic[index] - retail[index]) * 100.0 / retail[index]
            for index in range(3)
        )
        if any(
            abs(calculated_delta[index] - reported_delta[index]) > 0.01
            for index in range(3)
        ):
            raise RuntimeError(
                f"inconsistent water benchmark deltas in {path.name}"
            )
        benchmarks.append(
            {
                "retail_samples": retail_samples,
                "realistic_samples": realistic_samples,
                "retail": retail,
                "realistic": realistic,
                "delta": reported_delta,
            }
        )
    return benchmarks


def report_water_benchmarks(log_paths, requested):
    benchmarks = read_water_benchmarks(log_paths)
    if not requested:
        if benchmarks:
            raise RuntimeError(
                "water benchmark summary appeared without --water-benchmark"
            )
        return
    if not benchmarks:
        raise RuntimeError(
            "water benchmark requested, but no boot completed the ABBA sample"
        )

    def delta_range(index):
        values = [benchmark["delta"][index] for benchmark in benchmarks]
        if len(values) == 1:
            return f"{values[0]:+.3f}%"
        return f"{min(values):+.3f}% to {max(values):+.3f}%"

    total_samples = sum(
        benchmark["retail_samples"] + benchmark["realistic_samples"]
        for benchmark in benchmarks
    )
    print(
        f"Water benchmark: {len(benchmarks)} completed boot(s), "
        f"{total_samples} measured supported-water frames"
    )
    print(
        "  Same-scene command-buffer GPU deltas "
        f"(realistic versus retail): mean {delta_range(0)}, "
        f"median {delta_range(1)}, p95 {delta_range(2)}"
    )


def validate_water_capture_bitmap(path, width=None, height=None):
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"water capture is not a regular file: {path.name}")
    with open(path, "rb") as stream:
        header = stream.read(54)
    if len(header) != 54 or header[:2] != b"BM":
        raise RuntimeError(f"water capture has an invalid BMP header: {path.name}")
    file_size = struct.unpack_from("<I", header, 2)[0]
    data_offset = struct.unpack_from("<I", header, 10)[0]
    dib_size, bitmap_width, bitmap_height = struct.unpack_from("<III", header, 14)
    planes, bits = struct.unpack_from("<HH", header, 26)
    compression = struct.unpack_from("<I", header, 30)[0]
    if (
        dib_size != 40 or planes != 1 or bits != 24 or compression != 0 or
        not 1 <= bitmap_width <= 16384 or
        not 1 <= bitmap_height <= 16384
    ):
        raise RuntimeError(f"water capture BMP metadata is invalid: {path.name}")
    row_bytes = (bitmap_width * 3 + 3) & ~3
    expected_size = 54 + row_bytes * bitmap_height
    if (
        (width is not None and bitmap_width != width) or
        (height is not None and bitmap_height != height)
    ):
        raise RuntimeError(f"water capture BMP metadata is inconsistent: {path.name}")
    if (
        data_offset != 54 or file_size != expected_size or
        path.stat().st_size != expected_size
    ):
        raise RuntimeError(f"water capture BMP size is inconsistent: {path.name}")
    return bitmap_width, bitmap_height


def report_water_captures(log_paths, output, requested, water_preset,
                          follow_relaunches):
    captures = []
    partial_boots = 0
    expected_files = set()
    for index, path in enumerate(log_paths, 1):
        data = path.read_bytes()
        matches = list(WATER_CAPTURE_PATTERN.finditer(data))
        prefixes = data.count(WATER_CAPTURE_PREFIX)
        if prefixes != len(matches):
            raise RuntimeError(
                f"malformed water capture summary in {path.name}"
            )
        if len(matches) > 1:
            raise RuntimeError(
                f"duplicate water capture summaries in {path.name}"
            )
        stem = f"water-capture-{index:03d}" if follow_relaunches else "water-capture"
        files = sorted(output.glob(f"{stem}-*.bmp"))
        trigger = output / f".{stem}.trigger"
        if trigger.exists() or trigger.is_symlink():
            raise RuntimeError(f"water capture trigger remained after {path.name}")
        if not matches:
            if len(files) >= WATER_CAPTURE_IMAGES:
                raise RuntimeError(
                    f"incomplete water capture file count is invalid in {path.name}"
                )
            expected = [
                output / f"{stem}-{image:05d}.bmp"
                for image in range(len(files))
            ]
            if files != expected:
                raise RuntimeError(
                    f"partial water capture sequence is invalid in {path.name}"
                )
            dimensions = {
                validate_water_capture_bitmap(capture)
                for capture in expected
            }
            if len(dimensions) > 1:
                raise RuntimeError(
                    f"partial water capture dimensions changed in {path.name}"
                )
            if expected:
                partial_boots += 1
                expected_files.update(expected)
            continue

        preset_bytes, width_bytes, height_bytes = matches[0].groups()
        preset = preset_bytes.decode("ascii")
        width, height = int(width_bytes), int(height_bytes)
        if preset != water_preset:
            raise RuntimeError(
                f"water capture preset is inconsistent in {path.name}"
            )
        if not 1 <= width <= 16384 or not 1 <= height <= 16384:
            raise RuntimeError(
                f"water capture dimensions are invalid in {path.name}"
            )
        expected = [
            output / f"{stem}-{image:05d}.bmp"
            for image in range(WATER_CAPTURE_IMAGES)
        ]
        if files != expected:
            raise RuntimeError(
                f"water capture sequence is incomplete in {path.name}"
            )
        for capture in expected:
            validate_water_capture_bitmap(capture, width, height)
            expected_files.add(capture)
        captures.append(
            {"preset": preset, "width": width, "height": height,
             "paths": expected}
        )

    discovered = set(output.glob("water-capture-*.bmp"))
    if discovered != expected_files:
        raise RuntimeError("unexpected water capture files were produced")
    if not requested:
        if captures or partial_boots:
            raise RuntimeError(
                "water capture evidence appeared without --water-capture"
            )
        return []
    if not captures:
        raise RuntimeError(
            "water capture requested, but no boot completed the fixed sequence"
        )
    dimensions = {(capture["width"], capture["height"]) for capture in captures}
    if len(dimensions) != 1:
        raise RuntimeError("water capture dimensions changed between boots")
    width, height = dimensions.pop()
    print(
        f"Water captures: {len(captures)} completed boot(s), "
        f"{WATER_CAPTURE_IMAGES} private {water_preset} frames each at "
        f"{width}x{height}; {partial_boots} partial boot sequence(s) "
        f"validated and excluded"
    )
    return captures


def gpu_audit_paths(output, follow_relaunches, completed_boots, max_boots):
    if not follow_relaunches:
        path = output / "gpu-audit.txt"
        if completed_boots != 1:
            raise RuntimeError("GPU audit boot count is invalid")
        return [path] if path.is_file() else []

    paths = sorted(output.glob("gpu-audit-*.txt"))
    expected = [output / f"gpu-audit-{boot:03d}.txt"
                for boot in range(1, completed_boots + 1)]
    if (not 1 <= completed_boots <= max_boots or
            (paths and paths != expected)):
        raise RuntimeError("numbered GPU audits are out of sequence")
    return paths


def extract_ocean_fingerprint_counts(audit_paths):
    batches = overflow = mismatches = 0
    hashes = set()
    for path in audit_paths:
        summary = None
        file_hashes = set()
        file_row_batches = 0
        in_section = False
        with open(path, "rb") as stream:
            for line in stream:
                line = line.rstrip(b"\r\n")
                if line == OCEAN_FINGERPRINT_HEADER:
                    if in_section:
                        raise RuntimeError("GPU audit contains duplicate ocean fingerprint sections")
                    in_section = True
                    continue
                if not in_section:
                    continue
                match = OCEAN_FINGERPRINT_PATTERN.fullmatch(line)
                if match:
                    if summary is not None or file_hashes:
                        raise RuntimeError("GPU audit contains misplaced ocean fingerprint summary")
                    summary = tuple(int(value) for value in match.groups())
                    continue
                match = OCEAN_FINGERPRINT_HASH_PATTERN.fullmatch(line)
                if match:
                    fingerprint, count = match.groups()
                    row_batches = int(count)
                    if (summary is None or fingerprint in file_hashes or
                            not 1 <= row_batches <= MAX_OCEAN_MARKER_COUNT or
                            file_row_batches > MAX_OCEAN_MARKER_COUNT - row_batches):
                        raise RuntimeError("GPU audit contains invalid ocean fingerprint rows")
                    file_hashes.add(fingerprint)
                    file_row_batches += row_batches
                    continue
                raise RuntimeError("GPU audit contains malformed ocean fingerprint data")
        if not in_section or summary is None:
            raise RuntimeError("GPU audit is missing the ocean fingerprint summary")
        file_batches, file_hash_count, file_overflow, file_mismatches = summary
        if (file_hash_count != len(file_hashes) or file_hash_count > 32 or
                any(value > MAX_OCEAN_MARKER_COUNT
                    for value in (file_batches, file_overflow, file_mismatches)) or
                file_overflow > file_batches or file_mismatches > file_batches or
                file_row_batches > file_batches or
                file_row_batches + file_overflow != file_batches):
            raise RuntimeError("GPU audit contains inconsistent ocean fingerprint counts")
        for total, value in ((batches, file_batches), (overflow, file_overflow),
                             (mismatches, file_mismatches)):
            if total > MAX_OCEAN_MARKER_COUNT - value:
                raise RuntimeError("aggregate ocean fingerprint count is out of range")
        batches += file_batches
        overflow += file_overflow
        mismatches += file_mismatches
        hashes.update(file_hashes)
    return batches, len(hashes), overflow, mismatches


def report_ocean_fingerprint_counts(audit_paths):
    batches, hashes, overflow, mismatches = extract_ocean_fingerprint_counts(audit_paths)
    suffix = "" if audit_paths else " (no audit summary; zero observed fingerprint coverage)"
    print(f"Ocean fingerprints: {batches} batches, {hashes} distinct hashes, "
          f"{overflow} overflow batches, {mismatches} scope mismatches{suffix}")
    return batches, hashes, overflow, mismatches


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, help="separately built native executable")
    parser.add_argument("--prepared-tree", required=True, help="verified preparation root")
    parser.add_argument("--toolkit", required=True, help="clean pinned toolkit checkout")
    parser.add_argument("--output-dir", required=True, help="new private directory outside the repository")
    parser.add_argument("--seconds", type=int, default=45, help="single-boot deadline (1-300 seconds)")
    parser.add_argument("--edition", choices=EDITIONS, default="original",
                        help="guest edition (default: original)")
    parser.add_argument("--quincunx-approx", action="store_true", help="opt into the unverified approximation")
    parser.add_argument("--gpu-audit", action="store_true", help="write the private GPU usage summary")
    parser.add_argument(
        "--frame-timing", action="store_true",
        help="report Metal flip timing without enabling the full GPU audit"
    )
    parser.add_argument(
        "--pipeline-timing", action="store_true",
        help="report bounded Metal pipeline lifecycle JSONL"
    )
    parser.add_argument(
        "--pipeline-trace", action="store_true",
        help="write a bounded private canonical-key .bwmpa artifact"
    )
    parser.add_argument(
        "--pipeline-warmup",
        help="preload a validated private canonical-key .bwmpa artifact"
    )
    parser.add_argument(
        "--profile", action="store_true",
        help="write the built-in native sampling profile without enabling GPU audit"
    )
    parser.add_argument("--profile-from", type=float,
                        help="start profiling this many seconds after each boot")
    parser.add_argument("--profile-to", type=float,
                        help="stop profiling this many seconds after each boot")
    parser.add_argument(
        "--profile-callers", action="store_true",
        help="attribute sampled system waits to their first native caller"
    )
    parser.add_argument(
        "--render-scale",
        help="force the Metal internal scale to auto or a value from 0.5 through 10"
    )
    parser.add_argument("--frame-cap", type=int, choices=(30, 60, 120),
                        help="force the Remastered high-frame-rate target")
    parser.add_argument("--water-preset", choices=("retail", "realistic"), default="retail",
                        help="Metal ocean preset (default: retail; realistic requires Remastered)")
    parser.add_argument(
        "--water-benchmark", action="store_true",
        help="run the diagnostic-only same-scene Retail/Realistic ABBA GPU sample"
    )
    parser.add_argument(
        "--water-capture", action="store_true",
        help="write four private fixed-offset supported-water BMP frames"
    )
    parser.add_argument("--disable-audio", action="store_true",
                        help="disable native audio for an isolated non-audio diagnostic")
    parser.add_argument("--headless-audio-audit", action="store_true",
                        help="use the silent aggregate-only 48 kHz audio diagnostic sink")
    parser.add_argument("--experimental-keyboard-hook", action="store_true",
                        help="opt into the unverified game-specific keyboard hook")
    parser.add_argument("--synthetic-input", action="store_true",
                        help="send synthetic SDL keys to a diagnostic build; no physical keys")
    parser.add_argument("--synthetic-input-script", choices=SYNTHETIC_INPUT_SCRIPTS,
                        help="run a built-in poll-epoch input script; no physical keys")
    parser.add_argument(
        "--guided-input-capture", action="store_true",
        help="enable a private live two-pad command file and game-frame BMP"
    )
    parser.add_argument("--follow-relaunches", action="store_true",
                        help="follow validated guest relaunches in separately sandboxed boots")
    parser.add_argument("--max-boots", type=int,
                        help="maximum boot count with --follow-relaunches (default 4, maximum 16)")
    parser.add_argument("--total-seconds", type=int,
                        help="total guest deadline with --follow-relaunches (default 300, maximum 3600)")
    parser.add_argument(
        "--save-seed",
        help="copy a bounded private save directory into the isolated run"
    )
    parser.add_argument(
        "--launch-seed",
        help="start from a validated private same-title relaunch record"
    )
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        parser.error("this diagnostic requires macOS")
    if not 1 <= args.seconds <= 300:
        parser.error("--seconds must be between 1 and 300")
    if args.synthetic_input and not args.experimental_keyboard_hook:
        parser.error("--synthetic-input requires --experimental-keyboard-hook")
    if args.synthetic_input_script and not args.experimental_keyboard_hook:
        parser.error("--synthetic-input-script requires --experimental-keyboard-hook")
    if args.synthetic_input and args.synthetic_input_script:
        parser.error("--synthetic-input and --synthetic-input-script are mutually exclusive")
    if args.guided_input_capture and (
            args.experimental_keyboard_hook or args.synthetic_input or
            args.synthetic_input_script):
        parser.error(
            "--guided-input-capture cannot use the experimental keyboard input path"
        )
    if args.disable_audio and args.headless_audio_audit:
        parser.error("--disable-audio and --headless-audio-audit are mutually exclusive")
    if (args.profile_from is not None or args.profile_to is not None) and not args.profile:
        parser.error("--profile-from and --profile-to require --profile")
    if args.profile_callers and not args.profile:
        parser.error("--profile-callers requires --profile")
    if args.profile_from is not None and args.profile_from < 0:
        parser.error("--profile-from must not be negative")
    if args.profile_to is not None and args.profile_to <= 0:
        parser.error("--profile-to must be positive")
    if (args.profile_from is not None and args.profile_to is not None and
            args.profile_to <= args.profile_from):
        parser.error("--profile-to must be after --profile-from")
    if args.render_scale is not None and args.render_scale != "auto":
        try:
            render_scale = float(args.render_scale)
        except ValueError:
            parser.error("--render-scale must be auto or a number from 0.5 through 10")
        if not math.isfinite(render_scale) or not 0.5 <= render_scale <= 10:
            parser.error("--render-scale must be auto or a number from 0.5 through 10")
    if bool(args.save_seed) != bool(args.launch_seed):
        parser.error("--save-seed and --launch-seed must be used together")
    if args.synthetic_input_script and args.edition != "remastered":
        parser.error("--synthetic-input-script requires --edition remastered")
    if args.water_preset == "realistic" and args.edition != "remastered":
        parser.error("--water-preset realistic requires --edition remastered")
    if args.water_benchmark and args.edition != "remastered":
        parser.error("--water-benchmark requires --edition remastered")
    if args.water_benchmark and args.water_preset != "retail":
        parser.error("--water-benchmark requires --water-preset retail")
    if args.water_benchmark and not args.gpu_audit:
        parser.error("--water-benchmark requires --gpu-audit")
    if args.water_capture and args.edition != "remastered":
        parser.error("--water-capture requires --edition remastered")
    if args.water_capture and not args.gpu_audit:
        parser.error("--water-capture requires --gpu-audit")
    if args.water_capture and args.water_benchmark:
        parser.error("--water-capture and --water-benchmark are mutually exclusive")
    if args.guided_input_capture and args.water_capture:
        parser.error("--guided-input-capture and --water-capture are mutually exclusive")
    if (args.water_capture and
            args.synthetic_input_script != SYNTHETIC_INPUT_SCRIPTS[0]):
        parser.error(
            "--water-capture requires the deterministic "
            "--synthetic-input-script"
        )
    if not args.follow_relaunches and (args.max_boots is not None or args.total_seconds is not None):
        parser.error("--max-boots and --total-seconds require --follow-relaunches")
    if args.max_boots is not None and not 1 <= args.max_boots <= MAX_RELAUNCH_BOOTS:
        parser.error("--max-boots must be between 1 and 16")
    if args.total_seconds is not None and not 1 <= args.total_seconds <= 3600:
        parser.error("--total-seconds must be between 1 and 3600")

    output = None
    try:
        executable = Path(args.exe).resolve(strict=True)
        prepared = Path(args.prepared_tree).resolve(strict=True)
        toolkit = Path(args.toolkit).resolve(strict=True)
        game_dir = (prepared / "game/disc").resolve(strict=True)
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise ValueError("executable is not an executable file")
        if prepared not in game_dir.parents:
            raise ValueError("prepared disc must be inside the prepared tree")
        if not (game_dir / "default.xbe").is_file():
            raise ValueError("prepared disc is missing default.xbe")
        pipeline_warmup = None
        if args.pipeline_warmup:
            warmup_input = Path(args.pipeline_warmup).expanduser()
            if warmup_input.is_symlink():
                raise ValueError("pipeline warm-up input must not be a symlink")
            pipeline_warmup = warmup_input.resolve(strict=True)
            warmup_state = pipeline_warmup.stat()
            if (not pipeline_warmup.is_file() or
                    pipeline_warmup.suffix != ".bwmpa" or
                    warmup_state.st_uid != os.geteuid() or
                    stat.S_IMODE(warmup_state.st_mode) & 0o077 or
                    warmup_state.st_nlink != 1):
                raise ValueError(
                    "pipeline warm-up input must be a private owner-only .bwmpa file"
                )
            if any(overlaps(pipeline_warmup, protected)
                   for protected in (ROOT, prepared, toolkit)):
                raise ValueError(
                    "pipeline warm-up input must be outside protected trees"
                )
        initial_launch = b""
        save_seed = None
        if args.save_seed:
            save_seed = Path(args.save_seed).expanduser().resolve()
            launch_seed = Path(args.launch_seed).expanduser().resolve()
            if not launch_seed.is_file():
                parser.error("--launch-seed must name a regular file")
            if launch_seed.stat().st_size > MAX_RELAUNCH_BYTES:
                parser.error("--launch-seed exceeds the relaunch-record limit")
            initial_launch = launch_seed.read_bytes()
            try:
                validate_relaunch(initial_launch)
            except RuntimeError as error:
                parser.error(f"--launch-seed is invalid: {error}")
        output = create_output(args.output_dir, prepared, toolkit)
        for name in ("home", "tmp", "config", "data", "saves"):
            (output / name).mkdir(mode=0o700)
        if save_seed is not None:
            try:
                save_files, save_bytes = copy_save_seed(save_seed, output / "saves")
            except ValueError as error:
                parser.error(str(error))
        if args.guided_input_capture:
            (output / "guided-pad.txt").touch(mode=0o600, exist_ok=False)
        profile = sandbox_profile(output)
        verify_prepared(prepared, game_dir)
        revision = toolkit_revision(toolkit)
        baseline = snapshot_tree(prepared)
        executable_hash = digest_file(executable)
        print("Executable SHA-256:", executable_hash)
        print("Diagnostic edition:", args.edition.capitalize())
        print("Diagnostic water preset:", args.water_preset)
        if args.water_benchmark:
            print("Diagnostic water benchmark: Retail/Realistic/Realistic/Retail")
        if args.water_capture:
            print("Diagnostic water capture: four private supported-frame BMPs")
        if args.guided_input_capture:
            print("Guided input:", output / "guided-pad.txt")
            print("Guided game frames:", output / "guided-frame-NNNNN.bmp")
        if args.frame_timing:
            print("Diagnostic frame timing: enabled without full GPU audit")
        if args.pipeline_timing:
            print("Diagnostic pipeline timing: bounded JSONL enabled")
        if args.pipeline_trace:
            print("Diagnostic pipeline trace: private bounded artifact enabled")
        if pipeline_warmup is not None:
            print("Diagnostic pipeline warm-up: private artifact enabled")
        if args.profile:
            print("Diagnostic sampling profile: enabled")
        if args.render_scale is not None:
            print("Diagnostic Metal render scale:", args.render_scale)
        if args.frame_cap is not None:
            print("Diagnostic frame cap:", args.frame_cap)
        if save_seed is not None:
            print(f"Diagnostic private save seed: {save_files} files, "
                  f"{save_bytes} bytes; validated same-title launch seed")
        if args.headless_audio_audit:
            print("Diagnostic audio: silent headless audit (not audible)")
        else:
            print("Diagnostic audio:", "disabled" if args.disable_audio else "enabled")
        result = None
        completed_boots = 0
        boot_limit_reached = False
        max_boots = args.max_boots if args.max_boots is not None else 4
        try:
            verify_sandbox(profile, output, game_dir / "default.xbe")
            if args.follow_relaunches:
                relaunch_result = run_guest_relaunches(
                    executable, output, game_dir, profile, args.seconds,
                    args.quincunx_approx, args.gpu_audit,
                    max_boots,
                    args.total_seconds if args.total_seconds is not None else 300,
                    keyboard_hook=args.experimental_keyboard_hook,
                    synthetic_input=args.synthetic_input, edition=args.edition,
                    disable_audio=args.disable_audio,
                    synthetic_input_script=args.synthetic_input_script,
                    audio_audit=args.headless_audio_audit,
                    water_preset=args.water_preset,
                    water_benchmark=args.water_benchmark,
                    water_capture=args.water_capture,
                    guided_input=args.guided_input_capture,
                    frame_timing=args.frame_timing,
                    pipeline_timing=args.pipeline_timing,
                    profile_enabled=args.profile,
                    profile_from=args.profile_from, profile_to=args.profile_to,
                    profile_callers=args.profile_callers,
                    render_scale=args.render_scale, frame_cap=args.frame_cap,
                    initial_launch=initial_launch,
                    pipeline_trace=args.pipeline_trace,
                    pipeline_warmup=pipeline_warmup)
                result = relaunch_result.guest_status
                completed_boots = relaunch_result.completed_boots
                boot_limit_reached = relaunch_result.boot_limit_reached
            else:
                result = run_guest(executable, output, game_dir, profile, args.seconds,
                                   args.quincunx_approx, args.gpu_audit,
                                   keyboard_hook=args.experimental_keyboard_hook,
                                   synthetic_input=args.synthetic_input, edition=args.edition,
                                   disable_audio=args.disable_audio,
                                   synthetic_input_script=args.synthetic_input_script,
                                   audio_audit=args.headless_audio_audit,
                                   water_preset=args.water_preset,
                                   water_benchmark=args.water_benchmark,
                                   water_capture=args.water_capture,
                                   guided_input=args.guided_input_capture,
                                   frame_timing=args.frame_timing,
                                   pipeline_timing=args.pipeline_timing,
                                   profile_enabled=args.profile,
                                   profile_from=args.profile_from,
                                   profile_to=args.profile_to,
                                   profile_callers=args.profile_callers,
                                   render_scale=args.render_scale,
                                   frame_cap=args.frame_cap,
                                   initial_launch=initial_launch,
                                   pipeline_trace=args.pipeline_trace,
                                   pipeline_warmup=pipeline_warmup)
                completed_boots = 1
        finally:
            changed = snapshot_tree(prepared) != baseline
            if toolkit_revision(toolkit) != revision or digest_file(executable) != executable_hash:
                raise RuntimeError("toolkit or executable changed during the diagnostic")
            if changed:
                raise RuntimeError("prepared tree changed during the diagnostic")
        print("Prepared-tree, toolkit and executable integrity unchanged")
        logs = guest_log_paths(output, args.follow_relaunches, completed_boots, max_boots)
        report_ocean_marker_counts(logs)
        report_water_semantic_counts(logs)
        report_realistic_water_coverage(
            logs, args.water_preset, args.water_benchmark
        )
        report_water_benchmarks(logs, args.water_benchmark)
        report_water_captures(
            logs, output, args.water_capture, args.water_preset,
            args.follow_relaunches
        )
        if args.gpu_audit:
            report_ocean_fingerprint_counts(
                gpu_audit_paths(output, args.follow_relaunches, completed_boots, max_boots))
        print(f"Guest boots: {completed_boots}; validated relaunches followed: "
              f"{completed_boots - 1 if args.follow_relaunches else 0}")
        print("Private diagnostic output:", output)
        if boot_limit_reached:
            print(f"Diagnostic bounded limit reached: --max-boots stopped after "
                  f"{completed_boots} completed boots while another relaunch was requested",
                  file=sys.stderr)
            return 1
        print("Guest exit status:", result, "(124 means its configured run limit was reached)")
        return result if result >= 0 else 128 - result
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("Diagnostic failed:", error, file=sys.stderr)
        if output is not None:
            print("Private diagnostic output:", output, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
