#!/usr/bin/env python3
"""Prepare graphics (one time): play a few short scenes, hidden, so the game's
pixel shaders are compiled and cached before the first real play.

    python tools/bw_warmup.py --exe <install>/build/Release/bloodwake.exe
        --game-dir <install>/game/disc [--work-dir <dir>] [--parallel 2]
        [--only <scene,...>] [--cache <folder>] [--list] [--progress json]

Why: the PC back end turns each of the game's graphics combiner states into a
pixel shader and compiles it the first time a frame needs it (15-60 ms each,
and a new scene needs a dozen at once), so a player's first minutes stutter.
Every compiled shader is kept in the shader cache (src/gpu/bw_pscache.c,
%LOCALAPPDATA%/xboxrecomp/shadercache), so this only has to happen once per
PC. This script makes that once happen during setup instead of during play.

How: each scene is a boot of the installed game with its window hidden and
its sound at -120 dB, straight into a level by a launch page this script
writes (below), for a fixed time (the game's own BW_RUN_SECONDS, with a hard
limit on top); a scene may follow the game's own reboot, as tools/play.py
does. Saved games go to the scene's own folder, deleted afterwards, so the
player's saves, settings and progress are never touched. (Original, like any
Original play, opens the console disk images and utility-drive caches in
%LOCALAPPDATA%/xboxrecomp; the toolkit keeps them there whatever the save
folder.) Besides those it writes only the shader cache. Nothing it makes
comes from the game: the launch pages are built from the fields below.

Settings: the shader text depends on some settings (anti-aliasing, the
Remastered sharpening of text and HUD edges...). If the player's settings
file exists, the scenes use it; otherwise they use what a fresh launcher
starts with (the default preset's values, else each setting's default),
written to the work folder.

--progress json prints {"step": "warmup", "pct": 0-100, "msg": ...} lines
(pct of the warm-up). It never fails a build: errors are reported and the
scene is skipped (exit 0; 1 only for bad arguments). Stdlib only.
"""
import argparse
import configparser
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

# ── The launch page ───────────────────────────────────────────────────────
# Blood Wake loads a level by rebooting into itself with a 3072-byte launch
# page (the port's main.c rebuilds it from a "BWRL" file, BW_LAUNCH_DATA_FILE).
# Its fields, as the port's own runs recorded them (meanings in brackets are
# our reading; "?" = not known, the value the menus wrote):
PAGE_SIZE = 3072
TAG = (0, b"SPLASH")            # the page's tag
MODE = 8                        # u32: 1 boots a level
SAVE_DEVICE = (24, b"U:\\")     # the save device
LEVEL = 600                     # ASCII: "01".."28" Story Mode missions, "sm_<arena>" Battle
HUMANS = 640                    # u8: human players (split screen)
STORY_FLAGS = {2556: 1, 2560: 1, 2680: 1, 2992: 5}     # u8s (?)
BATTLE_FLAGS = {544: 1, 2560: 1, 2680: 1, 2992: 5}     # u8s (?)
BATTLE_RULES = 592              # u8: the battle game (1, ...)
BATTLE_ARENA2 = 558             # ASCII: the arena again
BOATS = 644                     # boat records, 132 bytes each, one per human player:
BOAT_STRIDE = 132               #   +0 class (ASCII, 32), +32 model (ASCII, 32),
BOAT_TEAM = 128                 #   +64 the shown name (UTF-16, 32 chars), +128 u8 team
PADS = 1975                     # u8 per player: the controller port (0xFF none)
MENU_FLAGS = {544: 1, 625: 1, 628: 3, 632: 1, 676: 1}  # u8s (?): the attract demo's return
MENU_FROM = (636, b"attract_01")                       # the level it returns from


def _put(page, off, data):
    page[off:off + len(data)] = data


def _boat(page, i, cls, model, team):
    o = BOATS + BOAT_STRIDE * i
    _put(page, o, cls.encode("ascii"))
    _put(page, o + 32, model.encode("ascii"))
    _put(page, o + 64, model.encode("utf-16-le"))
    page[o + BOAT_TEAM] = team


def story_page(level):
    """Story Mode mission `level` ("01".."28"), one player, the first save slot."""
    p = bytearray(PAGE_SIZE)
    _put(p, *TAG)
    struct.pack_into("<I", p, MODE, 1)
    _put(p, *SAVE_DEVICE)
    _put(p, LEVEL, level.encode("ascii"))
    p[HUMANS] = 1
    for off, v in STORY_FLAGS.items():
        p[off] = v
    return bytes(p)


def title_page():
    """The title screen (mode 0, nothing else)."""
    p = bytearray(PAGE_SIZE)
    _put(p, *TAG)
    struct.pack_into("<I", p, MODE, 0)
    return bytes(p)


def menus_page():
    """Straight to the main menu, as the attract demo returns to it."""
    p = bytearray(title_page())
    for off, v in MENU_FLAGS.items():
        p[off] = v
    _put(p, *MENU_FROM)
    return bytes(p)


def battle_page(arena, humans, rules=1, boat=("Catamaran", "Hellcat")):
    """A Battle Mode game in `arena` for `humans` split-screen players, each in
    `boat`, alternate teams, controller ports 0..humans-1."""
    p = bytearray(PAGE_SIZE)
    _put(p, *TAG)
    struct.pack_into("<I", p, MODE, 1)
    _put(p, *SAVE_DEVICE)
    _put(p, LEVEL, arena.encode("ascii"))
    _put(p, BATTLE_ARENA2, arena.encode("ascii"))
    p[HUMANS] = humans
    p[BATTLE_RULES] = rules
    for off, v in BATTLE_FLAGS.items():
        p[off] = v
    for i in range(humans):
        _boat(p, i, boat[0], boat[1], i % 2)
    for i in range(4):
        p[PADS + i] = i if i < humans else 0xFF
    return bytes(p)


def launch_file(page, path):
    """The port's launch file (main.c): "BWRL", version 1, the image path, the page."""
    img = b"D:\\default.xbe"
    data = b"BWRL" + struct.pack("<II", 1, len(img)) + img + struct.pack("<I", len(page)) + page
    with open(path, "wb") as f:
        f.write(data)
    return path


# ── The scenes ────────────────────────────────────────────────────────────
# Chosen from per-scene shader sets measured with an empty cache: every
# mission, the Battle games, 2- and 4-player split screen and the menus, in
# both editions, 60 s each (68 boots, 44 different shaders, 25 of them the
# same in both editions, so Original only has to add its own 4). These seven
# scenes compile 43 of the 44 (plus one of the title's that no 60 s scene
# showed) in the least play time; the 44th came once, from enemy action
# against an idle boat, and the first play compiles it if it ever comes.
# `seconds` is the scene's limit from its first boot's start (so it includes
# the loads), with room for a slower PC: the last needed shader came at
# 3-17 s (title, menus, split), 33-38 s (missions 27 and 23) and, in
# mission 23 Original, right after the game's own reboot at the mission's
# end (about 45 s). `follow`: how many of the game's own reboots to follow.
SCENES = [
    # name, edition, page, pads, pad script, seconds, follow
    ("mission23", "original", story_page("23"), 1, None, 65, 1),
    ("mission23", "remastered", story_page("23"), 1, None, 50, 0),
    ("mission27", "original", story_page("27"), 1, None, 45, 0),
    ("title", "remastered", title_page(), 1, None, 25, 0),
    ("split4", "remastered", battle_page("sm_octopusbay", 4, rules=2), 4, "9000:start", 20, 0),
    ("menus", "remastered", menus_page(), 1, None, 15, 0),
    ("title", "original", title_page(), 1, None, 15, 0),
]


def scene_list():
    """The scenes, longest first (so two workers finish together)."""
    return [dict(zip(("name", "edition", "page", "pads", "script", "seconds", "follow"), s))
            for s in sorted(SCENES, key=lambda s: -s[5])]


# ── Settings ──────────────────────────────────────────────────────────────

def player_settings():
    base = os.environ.get("LOCALAPPDATA")
    p = os.path.join(base, "BloodWake", "settings.ini") if base else None
    return p if p and os.path.isfile(p) else None


def default_settings(exe, out):
    """What tools/launcher.pyw starts a new install with: the default preset's
    value for each setting, else the setting's default."""
    r = subprocess.run([exe, "--settings-schema"], capture_output=True, timeout=30,
                       cwd=os.path.dirname(out), creationflags=_NO_WINDOW)
    schema = json.loads(r.stdout.decode("utf-8", "replace"))
    pst = next((s for s in schema["settings"] if s["section"] == "edition" and s["key"] == "preset"), None)
    first = next((p for p in schema.get("presets", []) if pst and p["value"] == pst["default"]), None)
    first = dict(first["settings"]) if first else {}
    cp = configparser.ConfigParser(interpolation=None)
    cp.optionxform = str
    for st in schema["settings"]:
        if not cp.has_section(st["section"]):
            cp.add_section(st["section"])
        cp.set(st["section"], st["key"], first.get(st["section"] + "." + st["key"], st["default"]))
    with open(out, "w", encoding="utf-8") as f:
        f.write("; Blood Wake settings for the graphics warm-up (tools/bw_warmup.py):\n"
                "; what a new launcher starts with. Not the player's file.\n\n")
        cp.write(f)
    return out


# ── Running a scene ───────────────────────────────────────────────────────

_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
BASE_ENV = {                            # as tools/play.py plays
    "RECOMP_PB_EXEC": "1", "RECOMP_FB_WINDOW": "1", "RECOMP_VBLANK": "paced", "RECOMP_USB": "1",
    "RECOMP_TITLE_KEVENTS": "1", "RECOMP_AC97_READY": "1", "RECOMP_TRACE_BUDGET": "0",
    "BW_STDERR_BUFFER": "1048576",
    "BW_GPU": "d3d11", "RECOMP_DISPATCH_GATE": "1", "RECOMP_FLIP_PACING": "1",
}
PS_LINE = re.compile(r"this run: (\d+) compiled in game \((\d+) ms\), (\d+) from the cache")
BOOT_MARK = "\n==== warm-up boot %d (%d s left)\n"


def run_scene(sc, exe, game_dir, work, settings, cache, log_dir):
    """One scene: a hidden boot, then (sc["follow"]) the game's own reboots, as
    tools/play.py follows them, until the scene's time is up. Returns a result
    dict; never raises."""
    res = {"scene": sc["name"], "edition": sc["edition"], "compiled": 0, "cached": 0,
           "seconds": 0.0, "boots": 0, "error": None}
    t0 = time.time()
    reported = False
    cwd = None
    try:
        cwd = tempfile.mkdtemp(prefix="bw_warmup_", dir=work)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("RECOMP_", "BW_"))}
        env.update(BASE_ENV)
        env.update({"BW_UNATTENDED": "1", "BW_GAME_DIR": game_dir, "BW_EDITION": sc["edition"],
                    "RECOMP_SAVE_DIR": os.path.join(cwd, "saves"),
                    "BW_EDITION_SAVE_ROOT": os.path.join(cwd, "saves"),
                    "RECOMP_USB_PADS": str(sc["pads"]),
                    "BW_AUDIO_GAIN_DB": "-120",           # silent: setup is no place for game sound
                    "BW_SEED": "1"})                      # the same scenes on every PC
        if settings:
            env["BW_SETTINGS"] = settings
        if cache:
            env["BW_GPU_PSCACHE"] = cache
        if sc["script"]:
            env["RECOMP_PAD_SCRIPT"] = sc["script"]
        launch = launch_file(sc["page"], os.path.join(cwd, "launch.bin")) if sc["page"] else None
        relaunch = os.path.join(cwd, "bw_relaunch.bin")   # the game's own reboot request (main.c)
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0                                   # SW_HIDE: no window on the player's screen
        log = os.path.join(log_dir, "warmup_%s_%s.log" % (sc["edition"], sc["name"]))
        with open(log, "w", encoding="utf-8", errors="replace") as lf:
            for boot in range(1 + sc["follow"]):
                left = int(sc["seconds"] - (time.time() - t0))
                if left < 5:
                    break
                if launch:
                    env["BW_LAUNCH_DATA_FILE"] = launch
                env["BW_RUN_SECONDS"] = str(left)
                if os.path.exists(relaunch):
                    os.remove(relaunch)
                lf.write(BOOT_MARK % (boot + 1, left))
                lf.flush()
                p = subprocess.Popen([exe], cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=lf,
                                     stderr=subprocess.STDOUT, startupinfo=si, creationflags=_NO_WINDOW)
                try:
                    code = p.wait(timeout=left + 45)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
                    code = None
                    res["error"] = "stopped after %d s" % (left + 45)
                res["boots"] += 1
                lf.flush()
                if code not in (0, 124, None) and not res["error"]:   # 124: the run limit (main.c)
                    res["error"] = "exit code %s" % code
                if code is None or not os.path.exists(relaunch):
                    break
                launch = os.path.join(cwd, "launch_next.bin")
                os.replace(relaunch, launch)
        res["seconds"] = time.time() - t0
        with open(log, encoding="utf-8", errors="replace") as lf:
            text = lf.read()
        for boot_text in text.split(BOOT_MARK.split("%")[0])[1:]:    # each boot reports its own totals
            hits = PS_LINE.findall(boot_text)
            if hits:
                reported = True
                res["compiled"] += int(hits[-1][0])
                res["cached"] += int(hits[-1][2])
        if not reported:
            res["compiled"] = res["cached"] = None
    except Exception as e:                  # a warm-up never breaks the install
        res["error"] = "%s: %s" % (type(e).__name__, e)
        res["seconds"] = time.time() - t0
    finally:
        if cwd:
            shutil.rmtree(cwd, ignore_errors=True)
    return res


def warmup(exe, game_dir, work, parallel=2, only=None, cache=None, prog=None):
    """Run the scenes, `parallel` at a time. prog(frac, msg). Returns the results."""
    prog = prog or (lambda f, m: None)
    scenes = [s for s in scene_list() if not only or s["name"] in only]
    if not scenes:
        return []
    os.makedirs(work, exist_ok=True)
    log_dir = os.path.join(work, "logs")
    os.makedirs(log_dir, exist_ok=True)
    settings = None
    if not player_settings():
        try:
            settings = default_settings(exe, os.path.join(work, "warmup_settings.ini"))
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as e:
            prog(0, "the default settings couldn't be read (%s); using the game's own" % e)
    total = float(sum(s["seconds"] for s in scenes)) / max(1, parallel)
    t0 = time.time()
    results, lock, todo = [], threading.Lock(), list(scenes)

    def worker():
        while True:
            with lock:
                if not todo:
                    return
                sc = todo.pop(0)
            r = run_scene(sc, exe, game_dir, work, settings, cache, log_dir)
            with lock:
                results.append(r)

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(max(1, parallel))]
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        for t in threads:
            t.join(timeout=5)
        with lock:
            done = len(results)
        prog(min(0.97, (time.time() - t0) / (total + 20)),
             "preparing graphics: %d of %d scenes (%d s)" % (done, len(scenes), time.time() - t0))
    return results


def summary(results):
    """(shaders compiled, shader loads from the cache, the results with a problem)"""
    n = sum(r["compiled"] or 0 for r in results)
    c = sum(r["cached"] or 0 for r in results)
    bad = [r for r in results if r["error"] or r["compiled"] is None]
    return n, c, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe")
    ap.add_argument("--game-dir")
    ap.add_argument("--work-dir", help="for the scenes' temporary folders and logs (default: next to the exe)")
    ap.add_argument("--parallel", type=int, default=2)
    ap.add_argument("--only", help="these scenes (comma-separated names)")
    ap.add_argument("--cache", help="the shader cache folder (default: the game's, %%LOCALAPPDATA%%/xboxrecomp/shadercache)")
    ap.add_argument("--list", action="store_true", help="list the scenes and stop")
    ap.add_argument("--progress", choices=("text", "json"), default="text")
    a = ap.parse_args(argv)
    if a.list:
        for s in scene_list():
            print("%-14s %-10s %2d s  %d pad(s)" % (s["name"], s["edition"], s["seconds"], s["pads"]))
        return 0
    if not a.exe or not a.game_dir:
        ap.error("--exe and --game-dir are needed")

    def say(frac, msg, **extra):
        if a.progress == "json":
            d = {"step": "warmup", "pct": int(round(100 * frac)), "msg": msg}
            d.update(extra)
            print(json.dumps(d), flush=True)
        else:
            print("[%3d%%] %s" % (100 * frac, msg), flush=True)

    work = os.path.abspath(a.work_dir or os.path.join(os.path.dirname(os.path.abspath(a.exe)), "warmup"))
    t0 = time.time()
    res = warmup(os.path.abspath(a.exe), os.path.abspath(a.game_dir), work, a.parallel,
                 set(a.only.split(",")) if a.only else None, a.cache, say)
    n, c, bad = summary(res)
    for r in sorted(res, key=lambda r: (r["edition"], r["scene"])):
        say(1, "%s %s: %s compiled, %s from the cache, %d s%s" % (
            r["edition"], r["scene"], r["compiled"], r["cached"], r["seconds"],
            " (%s)" % r["error"] if r["error"] else ""))
    say(1, "graphics ready: %d shaders compiled, %d loads from the cache, in %d s%s" % (
        n, c, time.time() - t0, "; %d scene(s) had a problem (skipped)" % len(bad) if bad else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
