"""Blood Wake launcher: the settings window and a big Play button.

Open it with "Blood Wake.cmd" (or play.cmd with no options) in the repo folder.

  - Edition, Display, Graphics, Audio, Fixes, Controls and Advanced tabs
    (and any others the game lists), built from the game's own table of settings (bloodwake.exe
    --settings-schema; src/bw_settings.c), so this window and the game always
    agree on what each choice means. Hover over anything for a short
    explanation.
  - Remastered's presets (the Edition tab: Best Quality, the default; Balanced
    and Original; Best Performance) set the Display and Graphics tabs in one
    go. Change any of those settings afterwards and the preset shows Custom.
  - Your choices are saved to %LOCALAPPDATA%/BloodWake/settings.ini, which every
    boot of the game reads, and are remembered for next time.
  - Play saves and starts the game (tools/play.py: one window for the whole
    session, controllers detected). The launcher hides while you play and
    comes back when the game closes.

Stdlib only (tkinter). Nothing here changes the game: it writes the settings
file and starts tools/play.py.
"""
import configparser
import json
import os
import subprocess
import sys
import tkinter as tk
from tkinter import messagebox, ttk

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "Release", "bloodwake.exe")
PLAY = os.path.join(REPO, "tools", "play.py")
CREATE_NO_WINDOW = 0x08000000

EDITION_TEXT = {
    "original": "The game exactly as it shipped in 2001: the console's look, 30 frames a second, "
                "4:3, and its quirks and bugs too.",
    "remastered": "The same game, made for a modern PC: bugs fixed, widescreen, sharper picture, "
                  "smoother frame rate and cleaner sound, with the original art. The Display, "
                  "Graphics, Audio and Fixes tabs tune it.",
}
APPLIES = {"all": "every edition", "remastered": "Remastered"}
LATER = "Remastered"            # the editions that start from a copy of Original's saves
SCALE = 1.0     # display scaling (125% = 1.25), set in main(): sizes below are at 100%


def px(n):
    """A size in pixels at 100% display scaling, scaled to this display."""
    return int(n * SCALE + 0.5)


def python_console():
    """A console Python for play.py (hidden), even when this runs under pythonw."""
    exe = sys.executable
    if os.path.basename(exe).lower() == "pythonw.exe":
        cand = os.path.join(os.path.dirname(exe), "python.exe")
        if os.path.isfile(cand):
            return cand
    return exe


def load_schema():
    out = subprocess.run([EXE, "--settings-schema"], capture_output=True, timeout=20,
                         creationflags=CREATE_NO_WINDOW, cwd=REPO)
    return json.loads(out.stdout.decode("utf-8", "replace"))


class Tip:
    """A short explanation that appears while the mouse is over a widget."""

    def __init__(self, widget, text):
        self.w, self.text, self.top = widget, text, None
        widget.bind("<Enter>", self.show, add="+")
        widget.bind("<Leave>", self.hide, add="+")

    def show(self, _e=None):
        if self.top or not self.text:
            return
        x = self.w.winfo_rootx() + 12
        y = self.w.winfo_rooty() + self.w.winfo_height() + 4
        self.top = tk.Toplevel(self.w)
        self.top.wm_overrideredirect(True)
        self.top.wm_geometry("+%d+%d" % (x, y))
        tk.Label(self.top, text=self.text, justify="left", wraplength=px(380), background="#fffbe6",
                 relief="solid", borderwidth=1, padx=8, pady=6).pack()

    def hide(self, _e=None):
        if self.top:
            self.top.destroy()
            self.top = None


class Launcher:
    def __init__(self, root, schema):
        self.root, self.schema = root, schema
        self.path = schema.get("file") or os.path.join(
            os.environ.get("LOCALAPPDATA", REPO), "BloodWake", "settings.ini")
        self.settings = schema["settings"]
        self.vars, self.widgets, self.proc = {}, {}, None
        # Remastered's presets (D-021). A setting the file doesn't hold yet (a
        # new install) starts at the default preset's choice (Best Quality).
        self.presets = schema.get("presets", [])
        self.pst = next((s for s in self.settings
                         if s["section"] == "edition" and s["key"] == "preset"), None)
        first = next((p for p in self.presets if self.pst and p["value"] == self.pst["default"]),
                     None)
        self.first = dict(first["settings"]) if first else {}
        self.ini = configparser.ConfigParser(interpolation=None)
        self.ini.optionxform = str
        try:
            self.ini.read(self.path, encoding="utf-8")
        except (OSError, configparser.Error):
            pass

        root.title("Blood Wake")
        root.geometry("%dx%d" % (px(800), px(680)))
        root.minsize(px(720), px(620))
        head = ttk.Frame(root, padding=(16, 12, 16, 4))
        head.pack(fill="x")
        ttk.Label(head, text="Blood Wake", font=("Segoe UI", 20, "bold")).pack(anchor="w")
        ttk.Label(head, text="Choose how to play, then press Play. Your choices are remembered. "
                             "Hover over anything for an explanation.",
                  foreground="#555", wraplength=px(740)).pack(anchor="w")

        self.nb = ttk.Notebook(root)
        self.nb.pack(fill="both", expand=True, padx=12, pady=8)
        for tab in schema["tabs"]:
            frame = ttk.Frame(self.nb, padding=12)
            self.nb.add(frame, text=tab)
            if tab == "Edition":
                self.build_edition(frame)
            else:
                self.build_tab(frame, tab)
        self.build_advanced()

        foot = ttk.Frame(root, padding=(12, 4, 12, 12))
        foot.pack(fill="x")
        ttk.Button(foot, text="Restore defaults", command=self.defaults).pack(side="left")
        self.status = ttk.Label(foot, text="", foreground="#555")
        self.status.pack(side="left", padx=12)
        style = ttk.Style()
        style.configure("Play.TButton", font=("Segoe UI", 14, "bold"), padding=(24, 10))
        ttk.Button(foot, text="Play", style="Play.TButton", command=self.play).pack(side="right")
        self.match_preset()
        self.refresh()

    # ---- building the tabs ---------------------------------------------------
    def saved(self, st):
        val = self.ini.get(st["section"], st["key"], fallback=None)
        values = [o["value"] for o in st["options"]]
        return val if val in values else self.first.get(st["section"] + "." + st["key"],
                                                        st["default"])

    @staticmethod
    def label_of(st, value):
        return next((o["label"] for o in st["options"] if o["value"] == value), value)

    def current(self, key):
        st, var = self.vars[key]
        return next((o["value"] for o in st["options"] if o["label"] == var.get()), None)

    def build_edition(self, frame):
        st = next(s for s in self.settings if s["key"] == "edition")
        var = tk.StringVar(value=self.saved(st))
        self.vars["edition"] = (st, var)
        ttk.Label(frame, text="Edition", font=("Segoe UI", 12, "bold")).pack(anchor="w")
        for o in st["options"]:
            rb = ttk.Radiobutton(frame, text=o["label"], value=o["value"], variable=var,
                                 command=self.refresh)
            rb.pack(anchor="w", pady=(10, 0))
            ttk.Label(frame, text=EDITION_TEXT.get(o["value"], ""), wraplength=px(520),
                      foreground="#444").pack(anchor="w", padx=24)
        if self.pst and self.presets:
            st = self.pst
            lab = ttk.Label(frame, text=st["label"], font=("Segoe UI", 12, "bold"))
            lab.pack(anchor="w", pady=(18, 0))
            row = ttk.Frame(frame)
            row.pack(anchor="w", pady=(6, 0))
            var = tk.StringVar()
            box = ttk.Combobox(row, values=[self.label_of(st, p["value"]) for p in self.presets],
                               textvariable=var, state="readonly", width=24)
            box.pack(side="left")
            box.bind("<<ComboboxSelected>>", self.pick_preset)
            note = ttk.Label(row, text="", foreground="#888")
            note.pack(side="left", padx=8)
            tip = st["tip"] + "\n\nApplies to: %s." % APPLIES.get(st["editions"], st["editions"])
            Tip(lab, tip)
            Tip(box, tip)
            self.preset_text = ttk.Label(frame, text="", wraplength=px(520), foreground="#444")
            self.preset_text.pack(anchor="w", padx=24, pady=(4, 0))
            self.vars["edition.preset"] = (st, var)
            self.widgets["edition.preset"] = (lab, box, note)
        ttk.Label(frame, text="\nEach edition keeps its own saved games. The first time you play "
                             "%s, it starts with a copy of your Original saves." % LATER,
                  wraplength=px(540), foreground="#666").pack(anchor="w")

    def build_tab(self, frame, tab):
        frame.columnconfigure(1, weight=1)
        row = 0
        for st in self.settings:
            if st["tab"] != tab:
                continue
            labels = [o["label"] for o in st["options"]]
            cur = self.saved(st)
            var = tk.StringVar(value=next(o["label"] for o in st["options"] if o["value"] == cur))
            lab = ttk.Label(frame, text=st["label"])
            lab.grid(row=row, column=0, sticky="w", pady=5, padx=(0, 16))
            box = ttk.Combobox(frame, values=labels, textvariable=var, state="readonly", width=30)
            box.grid(row=row, column=1, sticky="w", pady=5)
            box.bind("<<ComboboxSelected>>", self.match_preset)
            note = ttk.Label(frame, text="", foreground="#888")
            note.grid(row=row, column=2, sticky="w", padx=8)
            tip = st["tip"] + "\n\nApplies to: %s." % APPLIES.get(st["editions"], st["editions"])
            Tip(lab, tip)
            Tip(box, tip)
            self.vars[st["section"] + "." + st["key"]] = (st, var)
            self.widgets[st["section"] + "." + st["key"]] = (lab, box, note)
            row += 1
        if row == 0:
            ttk.Label(frame, text="Nothing here yet.").grid(row=0, column=0)

    def build_advanced(self):
        frame = ttk.Frame(self.nb, padding=12)
        self.nb.add(frame, text="Advanced")
        ttk.Label(frame, text="Extra switches, one per line as NAME=value (for testing; the "
                             "usual choices are on the other tabs). Leave empty if unsure.",
                  wraplength=px(560), foreground="#444").pack(anchor="w")
        self.adv = tk.Text(frame, height=10, width=70, font=("Consolas", 10))
        self.adv.pack(fill="both", expand=True, pady=8)
        if self.ini.has_section("advanced"):
            self.adv.insert("1.0", "\n".join("%s=%s" % kv for kv in self.ini.items("advanced")))
        btns = ttk.Frame(frame)
        btns.pack(fill="x")
        ttk.Button(btns, text="Open the settings folder",
                   command=lambda: self.open_folder(os.path.dirname(self.path))).pack(side="left")
        ttk.Button(btns, text="Open the saved games folder",
                   command=self.open_saves).pack(side="left", padx=8)
        ttk.Button(btns, text="Open the game's log",
                   command=lambda: self.open_file(os.path.join(REPO, "play.log"))).pack(side="left")
        ttk.Label(frame, text="\nSettings file: %s" % self.path, foreground="#888",
                  wraplength=px(560)).pack(anchor="w")

    # ---- state ------------------------------------------------------------------
    def edition(self):
        return self.vars["edition"][1].get()

    def refresh(self):
        ed = self.edition()
        for key, (lab, box, note) in self.widgets.items():
            st = self.vars[key][0]
            ok = (st["editions"] == "all" or (st["editions"] == "remastered" and ed != "original")
                  or st["editions"] == ed)          # a setting for one edition only
            box.configure(state="readonly" if ok else "disabled")
            lab.configure(foreground="" if ok else "#999")
            note.configure(text="" if ok else "(%s only)" % APPLIES[st["editions"]])

    def defaults(self):
        for key, (st, var) in self.vars.items():
            if key == "edition":
                continue
            var.set(self.label_of(st, self.first.get(key, st["default"])))
        self.adv.delete("1.0", "end")
        self.match_preset()
        self.status.configure(text="Defaults restored (the edition is kept).")
        self.refresh()

    def pick_preset(self, _e=None):
        """A preset was picked: set every setting it names."""
        st, var = self.vars["edition.preset"]
        p = next((p for p in self.presets if self.label_of(st, p["value"]) == var.get()), None)
        for key, value in (p["settings"].items() if p else ()):
            if key in self.vars:
                var2 = self.vars[key][1]
                var2.set(self.label_of(self.vars[key][0], value))
        self.match_preset()
        self.status.configure(text="%s: the Display and Graphics tabs are set." % var.get())

    def match_preset(self, _e=None):
        """Show the preset the settings match, or Custom."""
        if "edition.preset" not in self.vars:
            return
        st, var = self.vars["edition.preset"]
        hit = next((p for p in self.presets
                    if all(k in self.vars and self.current(k) == v for k, v in p["settings"].items())),
                   None)
        var.set(self.label_of(st, hit["value"] if hit else "custom"))
        self.preset_text.configure(text=hit["text"] if hit else
                                   "Your own mix: a setting was changed after picking a preset.")

    def save(self):
        cp = configparser.ConfigParser(interpolation=None)
        cp.optionxform = str
        for key, (st, var) in self.vars.items():
            if key == "edition":
                value = var.get()
            else:
                value = next(o["value"] for o in st["options"] if o["label"] == var.get())
            if not cp.has_section(st["section"]):
                cp.add_section(st["section"])
            cp.set(st["section"], st["key"], value)
        lines = [l.strip() for l in self.adv.get("1.0", "end").splitlines()]
        adv = [l.split("=", 1) for l in lines if "=" in l and not l.startswith((";", "#"))]
        if adv:
            cp.add_section("advanced")
            for k, v in adv:
                cp.set("advanced", k.strip(), v.strip())
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as f:
            f.write("; Blood Wake settings, written by the launcher (tools/launcher.pyw).\n"
                    "; Every boot of the game reads this file; environment variables win.\n\n")
            cp.write(f)

    # ---- actions ------------------------------------------------------------------
    def play(self):
        if self.proc and self.proc.poll() is None:
            return
        try:
            self.save()
        except OSError as e:
            messagebox.showerror("Blood Wake", "Couldn't save the settings:\n%s" % e)
            return
        self.status.configure(text="Starting...")
        self.proc = subprocess.Popen([python_console(), PLAY], cwd=REPO,
                                     creationflags=CREATE_NO_WINDOW)
        self.root.after(1500, self.root.withdraw)
        self.root.after(2000, self.watch)

    def watch(self):
        if self.proc and self.proc.poll() is None:
            self.root.after(1000, self.watch)
            return
        self.root.deiconify()
        self.status.configure(text="The game closed. Press Play to start again.")

    @staticmethod
    def open_folder(path):
        os.makedirs(path, exist_ok=True)
        os.startfile(path)

    @staticmethod
    def open_file(path):
        if os.path.isfile(path):
            os.startfile(path)
        else:
            messagebox.showinfo("Blood Wake", "There's no log yet: play once first.")

    def open_saves(self):
        base = os.path.join(os.environ.get("LOCALAPPDATA", ""), "xboxrecomp")
        ed = self.edition()
        self.open_folder(base if ed == "original" else os.path.join(base, "editions", ed))


def main():
    global SCALE
    try:                                    # crisp text on a scaled display
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (OSError, AttributeError):
        pass
    root = tk.Tk()
    SCALE = max(1.0, root.winfo_fpixels("1i") / 96.0)
    if not os.path.isfile(EXE):
        root.withdraw()
        messagebox.showerror("Blood Wake", "The game isn't built yet:\n%s\n\nBuild it first "
                                           "(docs/SETUP.md)." % EXE)
        return 1
    try:
        schema = load_schema()
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        root.withdraw()
        messagebox.showerror("Blood Wake", "Couldn't read the game's settings table:\n%s" % e)
        return 1
    Launcher(root, schema)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
