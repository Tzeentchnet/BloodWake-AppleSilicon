#!/usr/bin/env python3
"""Run the xboxrecomp pipeline for Blood Wake, with every step logged.

One command instead of six, so a regeneration is repeatable and leaves
evidence behind (D-008, EVIDENCE_SUITE.md L1):

    py -3 tools/run_pipeline.py                    # full run: parse -> recomp -> metrics
    py -3 tools/run_pipeline.py --ghidra           # also run Ghidra FidDb naming before lifting
    py -3 tools/run_pipeline.py --from recomp      # resume from a step
    py -3 tools/run_pipeline.py --xbe ../xboxrecomp/tools/conformance/test.xbe --gen-dir analysis/dryrun_gen
                                                   # dry run on the toolkit's synthetic XBE

Steps (in the order GETTING_STARTED.md requires):
    parse    tools.xbe_parser --json <xbe dir>/default_analysis.json
    disasm   tools.disasm (ALL code sections -- never --text-only)
    funcid   tools.func_id
    abi      tools.abi_analysis   (skipping it silently corrupts every signature)
    ghidra   run_ghidra.sh + merge_names.py --apply   (only with --ghidra)
    recomp   tools.recomp --all --split 250 --gen-dir src/recomp/gen, run through
             tools/xbr_patches.py, which fixes known toolkit bugs in memory (D-010;
             --no-toolkit-fixes runs it as shipped)
    doctor   tools/doctor.py --json
    metrics  tools/bw_static_metrics.py

Each run writes analysis/pipeline/<timestamp>/ (git-ignored: it holds output
derived from the game) with one log per step and manifest.json recording the
exact commands, exit codes, durations, tool versions and the XBE's hash.

From a clean tree (tools/build_from_disc.py, D-019): --seeds FILE gives the
disassembler the tracked function seeds (names/icall_seeds.json) without the
indirect-call sites, --evidence-dir puts the run folder elsewhere,
--no-cached-names never re-applies a cached Ghidra export, --no-names leaves
names/bw_names.json out (the public build has no names file), and a toolkit
without .git is accepted when build_from_disc's .bw_toolkit.json (written
after it checked the archive's SHA-256) names the pinned commit.

Stdlib only. Runs on Windows (py -3) and Linux/macOS (python3).
"""

import argparse
import datetime as _dt
import glob
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PINNED_COMMIT = "f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b"   # xboxrecomp v0.16.0 (D-002)
STEPS = ["parse", "disasm", "funcid", "abi", "ghidra", "recomp", "doctor", "metrics"]


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(args, cwd):
    try:
        r = subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else None
    except OSError:
        return None


TOOLKIT_MARKER = ".bw_toolkit.json"     # tools/build_from_disc.py, after the archive's SHA-256


def toolkit_identity(tk):
    """(commit, describe, dirty_tracked, how) of the toolkit folder. A git
    checkout answers for itself; an extracted archive has no .git (and git
    would then answer for whatever repository encloses the folder), so its
    commit comes from build_from_disc's marker."""
    if os.path.exists(os.path.join(tk, ".git")):
        return (git(["rev-parse", "HEAD"], tk), git(["describe", "--tags", "--always"], tk),
                bool(git(["status", "--porcelain", "--untracked-files=no"], tk)), "git")
    try:
        with open(os.path.join(tk, TOOLKIT_MARKER), encoding="utf-8") as f:
            m = json.load(f)
        return m.get("commit"), m.get("describe"), False, "archive " + str(m.get("archive_sha256"))
    except (OSError, ValueError):
        return None, None, False, "unknown"


def find_bash():
    """Git Bash on Windows; plain bash elsewhere."""
    if os.name == "nt":
        for c in (os.environ.get("CLAUDE_CODE_GIT_BASH_PATH"),
                  r"C:\Program Files\Git\bin\bash.exe",
                  r"C:\Program Files (x86)\Git\bin\bash.exe"):
            if c and os.path.exists(c):
                return c
    return shutil.which("bash")


def module_version(name):
    """The installed distribution's version (capstone's own __version__ reports its C core)."""
    try:
        from importlib.metadata import version
        return version(name)
    except Exception:
        try:
            __import__(name)
            return "present"
        except Exception:
            return None


DATA_SECTION_NAMES = {".data", ".data1", ".rdata", ".idata", ".edata", ".reloc", ".tls"}


class Runner:
    def __init__(self, args):
        self.a = args
        self.tk = os.path.abspath(args.toolkit)
        self.xbe = os.path.abspath(args.xbe)
        self.gen = os.path.abspath(args.gen_dir)
        stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        base = os.path.join(os.path.abspath(args.evidence_dir) if args.evidence_dir
                            else os.path.join(REPO, "analysis", "pipeline"), stamp)
        self.run_dir, n = base, 1
        while os.path.exists(self.run_dir):          # never overwrite an earlier run's evidence
            n += 1
            self.run_dir = "%s-%d" % (base, n)
        os.makedirs(self.run_dir)
        self.manifest = {
            "started": _dt.datetime.now().isoformat(timespec="seconds"),
            "host": {"platform": platform.platform(), "python": sys.version.split()[0],
                     "executable": sys.executable,
                     "capstone": module_version("capstone"), "pefile": module_version("pefile")},
            "repo": {"path": REPO, "commit": git(["rev-parse", "HEAD"], REPO),
                     "dirty": bool(git(["status", "--porcelain"], REPO))},
            "toolkit": dict(zip(("commit", "describe", "dirty_tracked", "identified_by"),
                                toolkit_identity(self.tk)), path=self.tk),
            "xbe": {"path": self.xbe},
            "args": vars(args),
            "steps": [],
            "flags": [],
        }

    def save(self):
        with open(os.path.join(self.run_dir, "manifest.json"), "w", encoding="utf-8") as f:
            json.dump(self.manifest, f, indent=2)

    def run(self, name, cmd, env_extra=None, cwd=None, label=None):
        log_path = os.path.join(self.run_dir, "%02d_%s.log" % (STEPS.index(name), label or name))
        env = dict(os.environ)
        env.update(env_extra or {})
        env["PYTHONIOENCODING"] = "utf-8"
        print("[%s] %s" % (name, " ".join(cmd)), flush=True)
        t0 = time.time()
        with open(log_path, "w", encoding="utf-8", errors="replace") as log:
            log.write("$ %s\n(cwd %s)\n\n" % (" ".join(cmd), cwd or self.tk))
            log.flush()
            # stdin closed: a tool that stops to ask (Ghidra wanting a JDK path) fails
            # instead of hanging an unattended run.
            p = subprocess.Popen(cmd, cwd=cwd or self.tk, env=env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                 encoding="utf-8", errors="replace")
            tail = []
            for line in p.stdout:
                log.write(line)
                tail.append(line)
                if len(tail) > 15:
                    tail.pop(0)
                if self.a.verbose:
                    sys.stdout.write(line)
            rc = p.wait()
        dt = round(time.time() - t0, 1)
        self.manifest["steps"].append({"step": name, "cmd": cmd, "exit": rc,
                                       "seconds": dt, "log": os.path.basename(log_path)})
        self.save()
        status = "ok" if rc == 0 else "FAILED (exit %d)" % rc
        print("[%s] %s in %.1fs -> %s" % (name, status, dt, log_path), flush=True)
        if rc != 0 and not self.a.verbose:
            sys.stdout.write("".join(tail))
        return rc, log_path

    def flag(self, msg):
        print("  !! " + msg, flush=True)
        self.manifest["flags"].append(msg)
        self.save()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--toolkit", default=os.path.join(os.path.dirname(REPO), "xboxrecomp"))
    ap.add_argument("--xbe", default=os.path.join(REPO, "game", "disc", "default.xbe"))
    ap.add_argument("--gen-dir", default=os.path.join(REPO, "src", "recomp", "gen"))
    ap.add_argument("--from", dest="start", choices=STEPS, default="parse")
    ap.add_argument("--to", dest="stop", choices=STEPS, default="metrics")
    ap.add_argument("--ghidra", action="store_true", help="run Ghidra FidDb naming (needs Ghidra 12.1.x + Jython extension)")
    ap.add_argument("--extra-sections", default=None,
                    help="passed to tools.disasm, e.g. DOLBY (use when disasm warns about unswept code)")
    ap.add_argument("--allow-other-toolkit", action="store_true",
                    help="run even if the toolkit is not at the pinned v0.16.0 commit")
    ap.add_argument("--icall", action="store_true",
                    help="use the indirect-call feedback in analysis/icall/ (tools/bw_icall.py): "
                         "seeds for disasm, per-site targets for recomp")
    ap.add_argument("--trace-functions", metavar="JSON",
                    help="compile entry/exit tracing into these functions, e.g. "
                         "tools/trace/boot_milestones.json (printed at runtime as [TRACE])")
    ap.add_argument("--no-toolkit-fixes", action="store_true",
                    help="run recomp exactly as the toolkit ships it, without tools/xbr_patches.py "
                         "(D-010; use to reproduce a toolkit bug)")
    ap.add_argument("--seeds", metavar="JSON",
                    help="function seeds for disasm only (--seed-functions), e.g. names/icall_seeds.json; "
                         "unlike --icall, recomp gets no indirect-call sites")
    ap.add_argument("--evidence-dir", metavar="DIR",
                    help="where the run folder goes (default analysis/pipeline)")
    ap.add_argument("--no-cached-names", action="store_true",
                    help="never re-apply a cached Ghidra export (a clean-tree build)")
    ap.add_argument("--no-names", action="store_true",
                    help="don't apply names/bw_names.json: functions keep their sub_ names "
                         "(the public build, D-019)")
    ap.add_argument("--dry-run", action="store_true",
                    help="rehearse on the toolkit's synthetic test.xbe (no game data); output under analysis/dryrun/")
    ap.add_argument("-v", "--verbose", action="store_true", help="stream every tool's output")
    a = ap.parse_args(argv)

    if a.dry_run:
        synth = os.path.join(a.toolkit, "tools", "conformance", "test.xbe")
        if not os.path.isfile(synth):
            print("ERROR: synthetic XBE not found at %s" % synth, file=sys.stderr)
            return 2
        dry = os.path.join(REPO, "analysis", "dryrun")
        os.makedirs(dry, exist_ok=True)
        a.xbe = os.path.join(dry, "default.xbe")
        shutil.copy2(synth, a.xbe)
        a.gen_dir = os.path.join(dry, "gen")
        print("DRY RUN on the synthetic test.xbe. Re-run without --dry-run for the real game.")

    if not os.path.isfile(a.xbe):
        print("ERROR: XBE not found: %s" % a.xbe, file=sys.stderr)
        return 2
    if not os.path.isdir(os.path.join(a.toolkit, "tools", "recomp")):
        print("ERROR: toolkit not found at %s" % a.toolkit, file=sys.stderr)
        return 2

    r = Runner(a)
    head = r.manifest["toolkit"]["commit"]
    if head != PINNED_COMMIT:
        msg = "toolkit HEAD is %s, not the pinned v0.16.0 (%s)" % (head, PINNED_COMMIT[:7])
        if not a.allow_other_toolkit:
            print("ERROR: " + msg + ". Check out v0.16.0 or pass --allow-other-toolkit.", file=sys.stderr)
            return 2
        r.flag(msg)
    if r.manifest["toolkit"]["dirty_tracked"]:
        r.flag("toolkit has modified tracked files -- results may not match a clean v0.16.0")
    if not r.manifest["host"]["capstone"]:
        print("ERROR: capstone is not installed for %s (pip install capstone==5.0.9)" % sys.executable,
              file=sys.stderr)
        return 2

    r.manifest["xbe"].update({"bytes": os.path.getsize(a.xbe), "sha1": sha1_of(a.xbe)})
    r.save()
    print("Run folder: %s" % r.run_dir)
    print("XBE: %s (%d bytes, sha1 %s)" % (a.xbe, r.manifest["xbe"]["bytes"], r.manifest["xbe"]["sha1"]))

    py = sys.executable
    xbe = r.xbe
    analysis = os.path.join(os.path.dirname(xbe),
                            os.path.splitext(os.path.basename(xbe))[0] + "_analysis.json")
    active = STEPS[STEPS.index(a.start):STEPS.index(a.stop) + 1]

    # Resuming after disasm reuses the toolkit's shared output folders. The toolkit's
    # own recomp check compares only file NAMES (both the dry run and the game are
    # default.xbe), so check the disassembler's recorded SHA-256 against this XBE.
    if STEPS.index(a.start) > STEPS.index("disasm"):
        cache_p = os.path.join(r.tk, "tools", "disasm", "output", ".disasm_cache.json")
        recorded = None
        if os.path.exists(cache_p):
            with open(cache_p, encoding="utf-8") as f:
                recorded = json.load(f).get("xbe_hash")
        h = hashlib.sha256()
        with open(xbe, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        if recorded != h.hexdigest():
            print("ERROR: the toolkit's disassembly is not from this XBE (another binary ran last).\n"
                  "       Start from parse or disasm instead of --from %s." % a.start, file=sys.stderr)
            return 2

    stamp_path = os.path.join(REPO, "analysis", "ghidra_names_stamp.json")
    icall_seeds = os.path.join(REPO, "analysis", "icall", "icall_seeds.json")
    icall_sites = os.path.join(REPO, "analysis", "icall", "icall_sites.json")
    ran_disasm = False

    for step in active:
        if step == "parse":
            rc, _ = r.run(step, [py, "-m", "tools.xbe_parser", xbe, "--json", analysis])
            if rc == 0 and os.path.exists(analysis):
                # tools.disasm never sweeps a non-executable section, and calls into one
                # become unresolved stubs. List the candidates for --extra-sections.
                with open(analysis, encoding="utf-8") as f:
                    secs = json.load(f).get("sections", [])
                for sec in secs:
                    if (not sec.get("executable") and sec.get("raw_size", 0) > 0
                            and sec.get("name") not in DATA_SECTION_NAMES):
                        r.flag("parse: section %s is marked non-executable, so disasm skips it. "
                               "If stubs land there (see metrics), re-run with --extra-sections %s"
                               % (sec.get("name"), sec.get("name")))
        elif step == "disasm":
            cmd = [py, "-m", "tools.disasm", xbe, "-v"]
            if a.extra_sections:
                cmd += ["--extra-sections", a.extra_sections]
            if a.seeds:
                seeds = os.path.abspath(a.seeds)
                if not os.path.isfile(seeds):
                    print("ERROR: --seeds file not found: %s" % seeds, file=sys.stderr)
                    return 2
                cmd += ["--seed-functions", seeds]
                r.manifest["seeds"] = {"file": seeds, "sha1": sha1_of(seeds)}
            elif a.icall:
                if os.path.isfile(icall_seeds):
                    cmd += ["--seed-functions", icall_seeds]
                    r.manifest.setdefault("icall", {})["seeds"] = {"file": icall_seeds,
                                                                   "sha1": sha1_of(icall_seeds)}
                else:
                    r.flag("disasm: --icall given but no %s yet (run tools/bw_icall.py seeds)"
                           % icall_seeds)
            rc, log = r.run(step, cmd)
            ran_disasm = True
            if rc == 0:
                with open(log, encoding="utf-8", errors="replace") as f:
                    for line in f:
                        if "unswept code" in line:
                            r.flag("disasm: " + line.strip() +
                                   "  -> re-run with --extra-sections <that section>")
        elif step == "funcid":
            rc, _ = r.run(step, [py, "-m", "tools.func_id", xbe, "-v"])
        elif step == "abi":
            rc, _ = r.run(step, [py, "-m", "tools.abi_analysis", xbe, "-v"])
        elif step == "ghidra":
            stamp = None
            if os.path.exists(stamp_path):
                with open(stamp_path, encoding="utf-8") as f:
                    stamp = json.load(f)
            same_xbe = bool(stamp) and stamp.get("xbe_sha1") == r.manifest["xbe"]["sha1"]
            if not a.ghidra and a.no_cached_names:
                continue
            if not a.ghidra:
                # disasm rewrites functions.json and drops the names. If Ghidra already ran
                # for THIS binary, re-apply its cached export (seconds, no re-analysis).
                if ran_disasm and same_xbe:
                    rc, _ = r.run(step, [py, "tools/ghidra_naming/merge_names.py", "--apply"],
                                  label="ghidra_merge_cached")
                    if rc == 0:
                        r.flag("ghidra: re-applied cached names from %s" % stamp.get("when"))
                else:
                    continue
            else:
                bash = find_bash()
                if not bash:
                    r.flag("ghidra: no bash found (install Git for Windows)")
                    rc = 1
                else:
                    # Forward slashes, and NO MSYS_NO_PATHCONV: run_ghidra.sh hands its own
                    # /c/... paths to `py -3`, and Git Bash must translate those for Windows.
                    xbe_fwd = xbe.replace("\\", "/")
                    rc, log = r.run(step, [bash, "tools/ghidra_naming/run_ghidra.sh"],
                                    env_extra={"XBE": xbe_fwd})
                    with open(log, encoding="utf-8", errors="replace") as f:
                        text = f.read()
                    if "Jython script failed" in text or "JythonStubException" in text:
                        r.flag("ghidra: Jython extension not installed (Ghidra: File > Install "
                               "Extensions > Jython, restart)")
                        rc = rc or 1
                    if rc == 0:
                        rc, _ = r.run(step, [py, "tools/ghidra_naming/merge_names.py", "--apply"],
                                      label="ghidra_merge")
                    if rc == 0:
                        with open(stamp_path, "w", encoding="utf-8") as f:
                            json.dump({"xbe_sha1": r.manifest["xbe"]["sha1"], "xbe": xbe,
                                       "when": _dt.datetime.now().isoformat(timespec="seconds")}, f)
        elif step == "recomp":
            os.makedirs(r.gen, exist_ok=True)
            # Our function names (names/bw_names.json) go into functions.json first, so
            # the generated C says Cheats_MatchCodes_0003F9E0 rather than sub_0003F9E0.
            # Hooked functions keep their sub_ names (tools/bw_names.py).
            names_json = os.path.join(REPO, "names", "bw_names.json")
            if a.no_names:
                r.manifest["names"] = "none (--no-names)"
            elif os.path.isfile(names_json) and not a.dry_run:
                nrc, _ = r.run(step, [py, os.path.join(REPO, "tools", "bw_names.py"),
                                      "--toolkit", r.tk, "apply"], label="names")
                if nrc != 0:
                    r.flag("names: bw_names.py apply failed; generating with sub_ names")
                r.manifest["names_sha1"] = sha1_of(names_json)
            recomp_args = [xbe, "--all", "--split", "250", "--game-name", "Blood Wake",
                           "--gen-dir", r.gen]
            # D-011: hooks in src/recomp_manual.c wrap generated bodies (sub_X -> sub_X_gen).
            # Edition hooks live in their own files, src/bw_<edition>_hooks.c (D-015/D-016).
            # The toolkit scans one file, so with any of those present it scans a
            # concatenation kept in this run's folder (each file's sha1 in the manifest).
            manual_c = os.path.join(REPO, "src", "recomp_manual.c")
            if os.path.isfile(manual_c) and not a.dry_run:
                hook_files = sorted(glob.glob(os.path.join(REPO, "src", "bw_*_hooks.c")))
                scan_c = manual_c
                if hook_files:
                    scan_c = os.path.join(r.run_dir, "manual_combined.c")
                    with open(scan_c, "w", encoding="utf-8") as out:
                        for p in [manual_c] + hook_files:
                            out.write("/* ---- %s ---- */\n"
                                      % os.path.relpath(p, REPO).replace(os.sep, "/"))
                            with open(p, encoding="utf-8", errors="replace") as f:
                                out.write(f.read())
                            out.write("\n")
                    r.manifest["hook_files_sha1"] = {
                        os.path.relpath(p, REPO).replace(os.sep, "/"): sha1_of(p)
                        for p in hook_files}
                recomp_args += ["--exclude-manual", scan_c]
                r.manifest["manual_c_sha1"] = sha1_of(manual_c)
            if a.icall:
                if os.path.isfile(icall_sites):
                    recomp_args += ["--icall-sites", icall_sites]
                    r.manifest.setdefault("icall", {})["sites"] = {"file": icall_sites,
                                                                   "sha1": sha1_of(icall_sites)}
                else:
                    r.flag("recomp: --icall given but no %s yet" % icall_sites)
            if a.trace_functions:
                tf = os.path.abspath(a.trace_functions)
                recomp_args += ["--trace-functions", tf]
                r.manifest["trace_functions"] = {"file": tf, "sha1": sha1_of(tf)}
                r.flag("recomp: entry tracing compiled in from %s" % os.path.basename(tf))
            if a.no_toolkit_fixes:
                r.flag("recomp: toolkit fixes OFF (--no-toolkit-fixes); known bugs will reappear")
                r.manifest["toolkit_fixes"] = []
                rc, _ = r.run(step, [py, "-m", "tools.recomp"] + recomp_args)
            else:
                # D-010: known v0.16.0 bugs fixed in memory. The toolkit files stay untouched,
                # and the wrapper refuses to run if the code it fixes has changed.
                sys.path.insert(0, os.path.join(REPO, "tools"))
                import xbr_patches
                r.manifest["toolkit_fixes"] = [{k: f[k] for k in ("id", "what", "target", "sha256")}
                                               for f in xbr_patches.FIXES]
                r.save()
                rc, _ = r.run(step, [py, os.path.join(REPO, "tools", "xbr_patches.py"),
                                     "--toolkit", r.tk, "recomp"] + recomp_args)
            src = os.path.join(r.tk, "tools", "recomp", "output", "summary.json")
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(r.run_dir, "recomp_summary.json"))
        elif step == "doctor":
            rc, _ = r.run(step, [py, "tools/doctor.py", "--json",
                                 os.path.join(r.run_dir, "doctor.json")])
        elif step == "metrics":
            out = os.path.join(r.run_dir, "static_metrics.json")
            rc, mlog = r.run(step, [py, os.path.join(REPO, "tools", "bw_static_metrics.py"),
                                    "--toolkit", r.tk, "--analysis", analysis,
                                    "--gen-dir", r.gen, "--out", out])
            with open(mlog, encoding="utf-8", errors="replace") as f:
                body = f.read().split("\n\n", 1)[-1]
            if not a.verbose:
                print(body.rstrip())
            if rc == 0 and (a.trace_functions or a.no_toolkit_fixes):
                # A traced or unpatched build isn't the canonical one; keep the trend clean.
                r.flag("metrics: not copied to metrics/ (traced or --no-toolkit-fixes run)")
            elif rc == 0 and not a.dry_run:
                # Counts only (no game bytes), so this copy is the one that gets committed.
                dst = os.path.join(REPO, "metrics", "static_%s.json" % _dt.date.today().isoformat())
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(out, dst)
                print("[metrics] committable copy -> %s" % dst)
        if rc != 0:
            r.flag("stopped at step '%s'" % step)
            r.manifest["finished"] = _dt.datetime.now().isoformat(timespec="seconds")
            r.manifest["result"] = "failed"
            r.save()
            print("\nPipeline FAILED at '%s'. Log: %s" % (step, r.run_dir))
            return 1

    r.manifest["finished"] = _dt.datetime.now().isoformat(timespec="seconds")
    r.manifest["result"] = "ok"
    r.save()
    print("\nPipeline finished. Evidence in %s" % r.run_dir)
    if r.manifest["flags"]:
        print("Flags to review:")
        for f in r.manifest["flags"]:
            print("  - " + f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
