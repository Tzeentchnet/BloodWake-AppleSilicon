#!/usr/bin/env python3
"""Blood Wake Recomp's setup helper. The installer runs it with the private
Python it adds to the install folder (python\\python.exe -I -u setup_helper.py ...).

    setup_helper.py deps --wheels <dir>
        pip from Python's own bundled wheel (offline), then the wheels Setup
        downloaded and SHA-256-checked (capstone, pefile, and CMake when
        Visual Studio has none), with no index and no other packages.
    setup_helper.py build --source <dir> [--stub] -- <build_from_disc.py arguments>
        tools/build_from_disc.py from the installed source, with a clean
        environment (no PYTHONHOME/PYTHONPATH, no per-user site-packages, so
        another Python on this PC can't change what runs). Its JSON progress
        lines pass straight through; the exit code is the script's.
        --stub (testing only) prints a short fake run instead and writes
        placeholder files, so the installer's pages, shortcuts and uninstaller
        can be tested in seconds.
    setup_helper.py record --install-json <file> key=value ...
        adds an "installer" block to install.json (what Setup added, so the
        uninstaller and a later Setup know).

Every line it prints itself is one JSON object, like build_from_disc.py's
--progress json: {"step": ..., "pct": 0-100, "msg": ...}. Stdlib only.
"""
import glob
import json
import os
import shutil
import subprocess
import sys
import time

PINS = {"capstone": "5.0.9", "pefile": "2024.8.26"}


def say(step, pct, msg, **extra):
    d = {"step": step, "pct": int(pct), "msg": msg}
    d.update(extra)
    sys.stdout.write(json.dumps(d) + "\n")
    sys.stdout.flush()


def clean_env():
    """This process's environment without anything that points Python elsewhere."""
    env = {k: v for k, v in os.environ.items()
           if not (k.upper() in ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "PYTHONUSERBASE",
                                 "PYTHONEXECUTABLE", "VIRTUAL_ENV", "CONDA_PREFIX", "__PYVENV_LAUNCHER__")
                   or k.upper().startswith("PIP_"))}
    env["PYTHONNOUSERSITE"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    scripts = os.path.join(os.path.dirname(sys.executable), "Scripts")
    env["PATH"] = scripts + os.pathsep + env.get("PATH", "")      # pip's cmake.exe, when used
    return env


def run_quiet(cmd, log):
    with open(log, "a", encoding="utf-8", errors="replace") as f:
        f.write("$ %s\n" % subprocess.list2cmdline(cmd))
        f.flush()
        return subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                              env=clean_env()).returncode


def installed(name):
    try:
        from importlib.metadata import version
        return version(name)
    except Exception:
        return None


def cmd_deps(a):
    wheels = sorted(glob.glob(os.path.join(a.wheels, "*.whl")))
    log = os.path.join(a.wheels, "pip.log")
    if not any(os.path.basename(w).lower().startswith("capstone-") for w in wheels):
        say("deps", 0, "capstone's wheel is missing from %s" % a.wheels, error=True, exit=3)
        return 3
    if installed("pip") is None:
        say("deps", 0, "setting up pip (from Python's own copy, offline)")
        if run_quiet([sys.executable, "-I", "-m", "ensurepip", "--default-pip"], log) != 0:
            say("deps", 0, "Python's pip setup failed. Log: %s" % log, error=True, exit=3)
            return 3
    say("deps", 50, "installing %s" % ", ".join(os.path.basename(w).split("-")[0] for w in wheels))
    rc = run_quiet([sys.executable, "-I", "-m", "pip", "install", "--isolated", "--no-index", "--no-deps",
                    "--disable-pip-version-check", "--no-warn-script-location", "--no-cache-dir",
                    "--upgrade"] + wheels, log)
    if rc != 0:
        say("deps", 0, "Installing the Python packages failed. Log: %s" % log, error=True, exit=3)
        return 3
    # A fresh process, so the check sees what pip just wrote.
    out = subprocess.run([sys.executable, "-I", "-c",
                          "import json; from importlib.metadata import version as v\n"
                          "print(json.dumps({n: v(n) for n in %r}))" % sorted(PINS)],
                         capture_output=True, text=True, env=clean_env())
    try:
        got = json.loads(out.stdout)
    except ValueError:
        got = {}
    bad = ["%s %s (found %s)" % (n, want, got.get(n)) for n, want in sorted(PINS.items()) if got.get(n) != want]
    if bad:
        say("deps", 0, "Wrong package versions after install: %s. Log: %s" % (", ".join(bad), log),
            error=True, exit=3)
        return 3
    say("deps", 100, "Python packages ready: %s" % ", ".join("%s %s" % kv for kv in sorted(got.items())))
    return 0


def stub_build(argv):
    """A fake build_from_disc run: the same JSON lines, placeholder files. Testing only."""
    inst = argv[argv.index("--install-dir") + 1]
    fail = os.environ.get("BW_SETUP_STUB_FAIL")
    steps = [("identify", 1, "a folder of the disc's files (stub)"), ("toolkit", 3, "xboxrecomp (stub)"),
             ("unpack", 10, "copying 3 of 17 files (stub)"), ("unpack", 18, "copying 17 of 17 files (stub)"),
             ("fingerprint", 20, "an unmodified retail copy of Blood Wake (stub)"),
             ("pipeline", 40, "translating the game's code: disasm (stub)"),
             ("pipeline", 68, "translated in 0 s (stub)"), ("verify", 70, "the generated code matches ours (stub)"),
             ("build", 85, "compiling (stub)"), ("build", 97, "built in 0 s (stub)"),
             ("stage", 99, "installed in %s (stub)" % inst)]
    for step, pct, msg in steps:
        if fail and step == "fingerprint":
            say("fingerprint", 0, "This isn't an unmodified retail copy of Blood Wake (stub failure).",
                error=True, exit=int(fail))
            return int(fail)
        say(step, pct, msg)
        time.sleep(0.4)
    for rel in ("build/Release/bloodwake.exe", "game/disc/stub.txt", "work/logs/stub.log"):
        p = os.path.join(inst, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write("placeholder written by setup_helper.py --stub\n")
    src = argv[argv.index("--source") + 1] if "--source" in argv else None
    os.makedirs(os.path.join(inst, "tools"), exist_ok=True)
    if src:
        for name in ("launcher.pyw", "play.py"):
            if os.path.isfile(os.path.join(src, "tools", name)):
                shutil.copy2(os.path.join(src, "tools", name), os.path.join(inst, "tools", name))
    with open(os.path.join(inst, "install.json"), "w", encoding="utf-8") as f:
        json.dump({"format": 1, "stub": True}, f, indent=2)
    say("done", 100, "Blood Wake is built (stub).")
    return 0


def cmd_build(a, rest):
    script = os.path.join(a.source, "tools", "build_from_disc.py")
    if a.stub:
        return stub_build(rest + ["--source", a.source])
    if not os.path.isfile(script):
        say("identify", 0, "The installed source is incomplete (no %s). Run Setup again." % script,
            error=True, exit=7)
        return 7
    cmd = [sys.executable, "-u", script] + rest + ["--progress", "json"]
    p = subprocess.Popen(cmd, cwd=a.source, env=clean_env(), stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace")
    for line in p.stdout:
        line = line.rstrip("\r\n")
        if line.startswith("{"):
            sys.stdout.write(line + "\n")
        elif line.strip():                       # anything else (a traceback): keep it, as JSON
            say("log", -1, line)
        sys.stdout.flush()
    return p.wait()


def cmd_record(a):
    try:
        with open(a.install_json, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError):
        d = {"format": 1}
    block = d.setdefault("installer", {})
    for kv in a.pairs:
        k, _, v = kv.partition("=")
        block[k] = True if v == "true" else False if v == "false" else v
    block["python"] = sys.version.split()[0]
    with open(a.install_json, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=2)
    say("record", 100, "recorded in %s" % a.install_json)
    return 0


def main(argv=None):
    import argparse
    argv = list(sys.argv[1:] if argv is None else argv)
    rest = []
    if "--" in argv:
        i = argv.index("--")
        argv, rest = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("deps")
    d.add_argument("--wheels", required=True)
    b = sub.add_parser("build")
    b.add_argument("--source", required=True)
    b.add_argument("--stub", action="store_true")
    r = sub.add_parser("record")
    r.add_argument("--install-json", required=True)
    r.add_argument("pairs", nargs="*")
    a = ap.parse_args(argv)
    if a.cmd == "deps":
        return cmd_deps(a)
    if a.cmd == "build":
        return cmd_build(a, rest)
    return cmd_record(a)


if __name__ == "__main__":
    sys.exit(main())
