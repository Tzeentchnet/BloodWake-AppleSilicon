"""Run one native guest boot in a private, bounded macOS sandbox."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import selectors
import stat
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
TOOLKIT_COMMIT = "f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b"
MAX_LOG_BYTES = 32 * 1024 * 1024
MAX_RELAUNCH_BYTES = 16 + 260 + 3072
SANDBOX = "/usr/bin/sandbox-exec"


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


def diagnostic_env(output, game_dir, seconds, quincunx, audit, input_fd, output_fd):
    env = {
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME": str(output / "home"),
        "TMPDIR": str(output / "tmp") + "/",
        "XDG_CONFIG_HOME": str(output / "config"),
        "XDG_DATA_HOME": str(output / "data"),
        "RECOMP_SAVE_DIR": str(output / "saves"),
        "BW_SETTINGS": "none",
        "BW_GAME_DIR": str(game_dir),
        "BW_EDITION": "original",
        "BW_RUN_SECONDS": str(seconds),
        "BW_NATIVE_CHILD": "1",
        "BW_NATIVE_LAUNCH_IN_FD": str(input_fd),
        "BW_NATIVE_LAUNCH_OUT_FD": str(output_fd),
    }
    if quincunx:
        env["BW_METAL_QUINCUNX_APPROX"] = "1"
    if audit:
        env["BW_GPU_AUDIT"] = "1"
        env["BW_GPU_AUDIT_EVERY"] = "5"
        env["BW_GPU_AUDIT_FILE"] = str(output / "gpu-audit.txt")
    return env


def capture_guest_output(child, output_read, log, timeout):
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
                        allowed = min(len(data), MAX_LOG_BYTES - log_size)
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


def run_guest(executable, output, game_dir, profile, seconds, quincunx, audit):
    input_read = input_write = output_read = output_write = -1
    child = None
    try:
        input_read, input_write = os.pipe()
        output_read, output_write = os.pipe()
        os.close(input_write)
        input_write = -1
        env = diagnostic_env(output, game_dir, seconds, quincunx, audit, input_read, output_write)
        with open(output / "guest.log", "xb") as log:
            child = subprocess.Popen(
                [SANDBOX, "-p", profile, str(executable), "--bw-native-child"],
                cwd=output, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                pass_fds=(input_read, output_write),
            )
            os.close(input_read)
            input_read = -1
            os.close(output_write)
            output_write = -1
            result, relaunch = capture_guest_output(child, output_read, log, seconds + 15)
        if relaunch:
            (output / "relaunch.bin").write_bytes(relaunch)
            print("Guest requested a relaunch; request saved privately, not executed")
        return result
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, help="separately built native executable")
    parser.add_argument("--prepared-tree", required=True, help="verified preparation root")
    parser.add_argument("--toolkit", required=True, help="clean pinned toolkit checkout")
    parser.add_argument("--output-dir", required=True, help="new private directory outside the repository")
    parser.add_argument("--seconds", type=int, default=45, help="single-boot deadline (1-300 seconds)")
    parser.add_argument("--quincunx-approx", action="store_true", help="opt into the unverified approximation")
    parser.add_argument("--gpu-audit", action="store_true", help="write the private GPU usage summary")
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        parser.error("this diagnostic requires macOS")
    if not 1 <= args.seconds <= 300:
        parser.error("--seconds must be between 1 and 300")

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
        output = create_output(args.output_dir, prepared, toolkit)
        for name in ("home", "tmp", "config", "data", "saves"):
            (output / name).mkdir(mode=0o700)
        profile = sandbox_profile(output)
        verify_prepared(prepared, game_dir)
        revision = toolkit_revision(toolkit)
        baseline = snapshot_tree(prepared)
        executable_hash = digest_file(executable)
        print("Executable SHA-256:", executable_hash)
        result = None
        try:
            verify_sandbox(profile, output, game_dir / "default.xbe")
            result = run_guest(executable, output, game_dir, profile, args.seconds,
                               args.quincunx_approx, args.gpu_audit)
        finally:
            changed = snapshot_tree(prepared) != baseline
            if toolkit_revision(toolkit) != revision or digest_file(executable) != executable_hash:
                raise RuntimeError("toolkit or executable changed during the diagnostic")
            if changed:
                raise RuntimeError("prepared tree changed during the diagnostic")
        print("Prepared-tree, toolkit and executable integrity unchanged")
        print("Private diagnostic output:", output)
        print("Guest exit status:", result, "(124 means its configured run limit was reached)")
        return result if result >= 0 else 128 - result
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("Diagnostic failed:", error, file=sys.stderr)
        if output is not None:
            print("Private diagnostic output:", output, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
