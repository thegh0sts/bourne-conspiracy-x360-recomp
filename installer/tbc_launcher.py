"""
The Bourne Conspiracy - PC launcher.

Shows the installer if the game isn't installed yet; otherwise a settings screen and a Play button.
On Play it:
  * rebuilds the game's packed config (Coalesced.ini) from the original copy with the chosen
    settings applied, so turning everything back to default always restores the original files;
  * starts tbc.exe with the matching ReXGlue command-line settings.

Settings are saved in %LOCALAPPDATA%\\BourneConspiracyPC\\settings.json.
"""

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import tbc_config as cfg
from tbc_installer import (ACCENT, APP_NAME, BG, DIM, FG, LAUNCHER_HOME, InstallView, install_info_path,
                           make_root, start_game)

VERSION = "0.1.0"
SETTINGS_PATH = LAUNCHER_HOME / "settings.json"
PANEL = "#1c2029"

RESOLUTIONS = ["Original (720p)", "1080p", "1440p", "4K"]
RES_FLAG = {"1080p": "1080p", "1440p": "1440p", "4K": "4k"}
RES_SIZE = {"1080p": (1920, 1080), "1440p": (2560, 1440), "4K": (3840, 2160)}
SCALES = ["1x (original)", "2x", "3x"]
AA = ["Off", "FXAA", "FXAA (strong)"]
AA_FLAG = {"FXAA": "fxaa", "FXAA (strong)": "fxaa_extreme"}
FPS = ["30", "60 (original)", "120 (experimental)", "144 (experimental)", "Unlimited (experimental)"]
FPS_CAP = {"30": ("30", 60), "60 (original)": ("62", 60), "120 (experimental)": ("120", 120),
           "144 (experimental)": ("144", 144), "Unlimited (experimental)": ("1000", 240)}

DEFAULTS = {
    "fullscreen": False,
    "resolution": "Original (720p)",
    "render_scale": "1x (original)",
    "aa": "Off",
    "vsync": True,
    "show_fps": False,
    "fps": "60 (original)",
    "motion_blur": True,
    "depth_of_field": True,
    "bloom": True,
    "fov": 100,
    "deadzone": 25,
    "close_on_play": True,
}


def load_settings():
    s = dict(DEFAULTS)
    try:
        s.update(json.loads(SETTINGS_PATH.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        pass
    return s


def save_settings(s):
    LAUNCHER_HOME.mkdir(parents=True, exist_ok=True)
    SETTINGS_PATH.write_text(json.dumps(s, indent=2), encoding="utf-8")


def load_install():
    try:
        info = json.loads(install_info_path().read_text(encoding="utf-8"))
        if Path(info["exe"]).is_file() and (Path(info["game"]) / "default.xex").is_file():
            return info
    except (OSError, ValueError, KeyError):
        pass
    return None


def save_install(exe, game, root=None):
    LAUNCHER_HOME.mkdir(parents=True, exist_ok=True)
    info = {"exe": str(exe), "game": str(game), "root": str(root or Path(exe).parent)}
    install_info_path().write_text(json.dumps(info, indent=2), encoding="utf-8")
    return info


# --------------------------------------------------------------------------
# Applying settings to the game's config
# --------------------------------------------------------------------------

def _scale_fov(body, factor):
    def repl(m):
        v = float(m.group(2)) * factor
        return f"{m.group(1)}={v:.1f}"
    return re.sub(r"(?m)^(Fov|ReverseFov)=([0-9]+(?:\.[0-9]+)?)\r?$", repl, body)


def apply_game_config(game: Path, s) -> str:
    """Rewrite Coalesced.ini from the original with settings applied. Returns a short summary."""
    conf = game / cfg.CONFIG_REL
    toc = game / cfg.TOC_REL
    conf_bak = conf.with_name(conf.name + ".bak")
    toc_bak = toc.with_name(toc.name + ".bak")
    if not conf_bak.exists():
        shutil.copy2(conf, conf_bak)
    if not toc_bak.exists():
        shutil.copy2(toc, toc_bak)

    cap, _ = FPS_CAP[s["fps"]]
    edits = []  # (file, section, key, value)
    if cap != "62":
        edits.append(("Xe-BourneEngine.ini", "Engine.GameEngine", "MaxSmoothedFrameRate", cap))
    if not s["motion_blur"]:
        edits.append(("Xe-BourneEngine.ini", "SystemSettings", "MotionBlur", "False"))
    if not s["depth_of_field"]:
        edits.append(("Xe-BourneEngine.ini", "SystemSettings", "DepthOfField", "False"))
    if not s["bloom"]:
        edits.append(("Xe-BourneEngine.ini", "SystemSettings", "Bloom", "False"))
    if int(s["deadzone"]) != 25:
        edits.append(("Xe-BourneInput.ini", "HM_Engine.HmPlayerInput", "DeadZone", f"{int(s['deadzone']) / 100:.2f}"))
    fov = int(s["fov"])

    if not edits and fov == 100:
        # Everything at default: put the original files back.
        shutil.copy2(conf_bak, conf)
        shutil.copy2(toc_bak, toc)
        return "original game config"

    files = cfg.decode(conf_bak.read_bytes())
    for short, section, key, value in edits:
        i = cfg.find_file(files, short)
        name, body = files[i]
        files[i] = (name, cfg.set_value(body, section, key, value))
    if fov != 100:
        for short in ("Xe-BourneCamera.ini", "BourneCamera.ini"):
            try:
                i = cfg.find_file(files, short)
            except SystemExit:
                continue
            name, body = files[i]
            files[i] = (name, _scale_fov(body, fov / 100))
    shutil.copy2(toc_bak, toc)  # update the TOC entry starting from the original
    cfg.write(game, files)
    return f"{len(edits) + (fov != 100)} config change(s)"


def window_size(s, screen):
    """Window size for windowed mode: the chosen output size, shrunk (16:9) to fit
    within 85% of the screen so a 1440p/4K window doesn't cover the whole display."""
    w, h = RES_SIZE.get(s["resolution"], (1280, 720))
    if screen:
        sw, sh = int(screen[0] * 0.85), int(screen[1] * 0.85)
        k = min(1.0, sw / w, sh / h)
        w, h = int(w * k) // 16 * 16, int(h * k) // 9 * 9
        w, h = min(w, h * 16 // 9), min(h, w * 9 // 16)
    return w, h


def game_args(s, screen=None):
    """ReXGlue command-line settings, only when not at default. Valued settings use the
    "--name value" form already used for --gpu_plugin; on/off settings use "--name=value"."""
    args = ["--gpu_plugin", "xenos"]
    # Always pass fullscreen explicitly: the runtime's own default is fullscreen, so
    # leaving the flag out when the box is unticked still opened full screen.
    args.append("--fullscreen=true" if s["fullscreen"] else "--fullscreen=false")
    if s["resolution"] in RES_FLAG:
        if s["fullscreen"]:
            args += ["--resolution", RES_FLAG[s["resolution"]]]
        else:
            # --resolution also sizes the window, so a 4K preset fills the screen even
            # when windowed. Set the game's output size and the window size separately.
            w, h = RES_SIZE[s["resolution"]]
            args += ["--video_mode_width", str(w), "--video_mode_height", str(h)]
    if not s["fullscreen"]:
        ww, wh = window_size(s, screen)
        args += ["--window_width", str(ww), "--window_height", str(wh)]
    scale = int(s["render_scale"][0])
    if scale > 1:
        args += ["--resolution_scale", str(scale)]
    if s["aa"] in AA_FLAG:
        args += ["--swap_post_effect", AA_FLAG[s["aa"]]]
    if not s["vsync"]:
        args.append("--vsync=false")
    if s.get("show_fps"):
        # Needs a tbc.exe built with the FPS counter (src/tbc_app.h); older builds
        # don't know this flag, so it's only passed when ticked.
        args.append("--show_fps=true")
    _, refresh = FPS_CAP[s["fps"]]
    if refresh != 60:
        args += ["--video_mode_refresh_rate", str(refresh)]
    return args


# --------------------------------------------------------------------------
# UI
# --------------------------------------------------------------------------

class SettingsView(tk.Frame):
    def __init__(self, master, info, on_reinstall, on_locate):
        super().__init__(master, bg=BG)
        self.info = info
        self.s = load_settings()
        self.vars = {}

        head = tk.Frame(self, bg=BG)
        head.pack(fill="x", padx=28, pady=(20, 4))
        tk.Label(head, text=APP_NAME.upper(), font=("Segoe UI", 22, "bold"), fg=ACCENT, bg=BG).pack(side="left")
        tk.Label(head, text=f"Launcher {VERSION}", font=("Segoe UI", 9), fg=DIM, bg=BG).pack(side="right", anchor="s")

        box = self._section("Game files")
        tk.Label(box, text=f"Installed in {info['game']}", font=("Segoe UI", 9), fg=DIM, bg=PANEL,
                 anchor="w").grid(row=0, column=0, columnspan=2, sticky="w")
        links = tk.Frame(box, bg=PANEL)
        links.grid(row=1, column=0, columnspan=2, sticky="w", pady=(4, 0))
        self._link(links, "Reinstall from a disc image...", on_reinstall).pack(side="left")
        self._link(links, "Use a different install...", on_locate).pack(side="left", padx=14)

        box = self._section("Display")
        self._check(box, 0, "fullscreen", "Fullscreen")
        self._combo(box, 1, "resolution", "Output resolution", RESOLUTIONS)
        self._combo(box, 2, "render_scale", "Render scale (sharper, slower)", SCALES)
        self._combo(box, 3, "aa", "Anti-aliasing", AA)
        self._combo(box, 4, "fps", "Frame rate limit", FPS)
        self._check(box, 5, "vsync", "VSync")
        self._check(box, 6, "show_fps", "Show FPS counter")

        box = self._section("Graphics and camera")
        self._check(box, 0, "motion_blur", "Motion blur")
        self._check(box, 1, "depth_of_field", "Depth of field")
        self._check(box, 2, "bloom", "Bloom")
        self._slider(box, 3, "fov", "Field of view (%)", 80, 140, 5)

        box = self._section("Controller")
        self._slider(box, 0, "deadzone", "Stick dead zone (%)", 5, 40, 1)

        foot = tk.Frame(self, bg=BG)
        foot.pack(side="bottom", fill="x", padx=28, pady=18)
        tk.Button(foot, text="Play", width=16, command=self.play, font=("Segoe UI", 13, "bold"), bg=ACCENT,
                  fg="white", activebackground="#e0203f", activeforeground="white", bd=0, pady=8).pack(side="left")
        tk.Button(foot, text="Quit", width=10, command=self.winfo_toplevel().destroy, font=("Segoe UI", 12),
                  bd=0, pady=8).pack(side="left", padx=10)
        tk.Button(foot, text="Defaults", width=10, command=self.reset, font=("Segoe UI", 11),
                  bd=0, pady=6).pack(side="right")
        self.vars["close_on_play"] = tk.BooleanVar(value=self.s["close_on_play"])
        tk.Checkbutton(foot, text="Close launcher when the game starts", variable=self.vars["close_on_play"],
                       font=("Segoe UI", 9), fg=DIM, bg=BG, selectcolor=BG, activebackground=BG,
                       activeforeground=FG).pack(side="right", padx=10)

        self.status = tk.StringVar(value="Experimental options may cause glitches. "
                                         "Set them back to default to restore the original game.")
        tk.Label(self, textvariable=self.status, font=("Segoe UI", 9), fg=DIM, bg=BG, wraplength=700,
                 justify="left").pack(side="bottom", anchor="w", padx=28)

    # widgets ------------------------------------------------------------
    def _section(self, title):
        tk.Label(self, text=title, font=("Segoe UI", 11, "bold"), fg=FG, bg=BG).pack(anchor="w", padx=28,
                                                                                     pady=(12, 3))
        box = tk.Frame(self, bg=PANEL, padx=14, pady=10)
        box.pack(fill="x", padx=28)
        box.columnconfigure(1, weight=1)
        return box

    def _link(self, parent, text, cmd):
        return tk.Button(parent, text=text, command=cmd, font=("Segoe UI", 9, "underline"), fg=FG, bg=PANEL,
                         activebackground=PANEL, activeforeground=ACCENT, bd=0, cursor="hand2")

    def _check(self, box, row, key, text):
        v = self.vars[key] = tk.BooleanVar(value=self.s[key])
        tk.Checkbutton(box, text=text, variable=v, font=("Segoe UI", 10), fg=FG, bg=PANEL, selectcolor=PANEL,
                       activebackground=PANEL, activeforeground=FG, anchor="w"
                       ).grid(row=row, column=0, columnspan=2, sticky="w")

    def _combo(self, box, row, key, text, values):
        v = self.vars[key] = tk.StringVar(value=self.s[key] if self.s[key] in values else DEFAULTS[key])
        tk.Label(box, text=text, font=("Segoe UI", 10), fg=FG, bg=PANEL).grid(row=row, column=0, sticky="w",
                                                                             pady=2)
        ttk.Combobox(box, textvariable=v, values=values, state="readonly", width=26
                     ).grid(row=row, column=1, sticky="e", pady=2)

    def _slider(self, box, row, key, text, lo, hi, step):
        v = self.vars[key] = tk.IntVar(value=int(self.s[key]))
        tk.Label(box, text=text, font=("Segoe UI", 10), fg=FG, bg=PANEL).grid(row=row, column=0, sticky="w")
        tk.Scale(box, variable=v, from_=lo, to=hi, resolution=step, orient="horizontal", length=230,
                 bg=PANEL, fg=FG, troughcolor="#2a2f38", highlightthickness=0, bd=0, activebackground=ACCENT
                 ).grid(row=row, column=1, sticky="e")

    # actions --------------------------------------------------------------
    def collect(self):
        for k, v in self.vars.items():
            self.s[k] = v.get()
        return self.s

    def reset(self):
        for k, v in self.vars.items():
            v.set(DEFAULTS[k])

    def play(self):
        s = self.collect()
        save_settings(s)
        exe, game = Path(self.info["exe"]), Path(self.info["game"])
        try:
            summary = apply_game_config(game, s)
        except PermissionError:
            messagebox.showerror("Play", "Couldn't update the game's config files. Is the game already running?")
            return
        except Exception as e:  # keep the launcher usable even if config editing fails
            if not messagebox.askyesno("Play", f"Couldn't apply the config settings ({e}).\n\n"
                                               "Start the game with its original config instead?"):
                return
            summary = "config unchanged"
        log = Path(self.info.get("root", exe.parent)) / "game.log"
        args = [str(exe), "--game_data_root", str(game)] + game_args(s, (self.winfo_screenwidth(), self.winfo_screenheight())) + ["--log_file", str(log)]
        try:
            start_game(args, exe.parent)
        except OSError as e:
            messagebox.showerror("Play", f"Couldn't start the game: {e}")
            return
        self.status.set(f"Started with {summary}.")
        if s["close_on_play"]:
            self.after(800, self.winfo_toplevel().destroy)


class Launcher:
    def __init__(self):
        self.root = make_root(APP_NAME)
        self.root.geometry("780x890")
        self.root.minsize(700, 760)
        self.view = None
        info = load_install()
        if info:
            self.show_settings(info)
        else:
            self.show_install(first_time=True)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def _swap(self, view):
        if self.view:
            self.view.destroy()
        self.view = view
        view.pack(fill="both", expand=True)

    def show_settings(self, info):
        self._swap(SettingsView(self.root, info, on_reinstall=lambda: self.show_install(), on_locate=self.locate))

    def show_install(self, first_time=False):
        info = load_install()
        default = Path(info["root"]) if info and info.get("root") else None
        back = None if first_time else (lambda: self.show_settings(load_install()))
        view = InstallView(self.root, on_installed=self.installed, on_back=back, default_dir=default)
        if first_time:
            self._link_locate(view)
        self._swap(view)

    def _link_locate(self, view):
        tk.Button(view, text="Already installed? Locate tbc.exe...", command=self.locate,
                  font=("Segoe UI", 9, "underline"), fg=DIM, bg=BG, bd=0, activebackground=BG,
                  activeforeground=ACCENT, cursor="hand2").pack(anchor="w", padx=28, pady=(8, 0))

    def installed(self, exe, game, root):
        self.show_settings(save_install(exe, game, root))

    def locate(self):
        exe = filedialog.askopenfilename(title="Locate tbc.exe", filetypes=[("tbc.exe", "tbc.exe"),
                                                                            ("Programs", "*.exe")])
        if not exe:
            return
        game = filedialog.askdirectory(title="Choose the extracted game folder (the one with default.xex)")
        if not game:
            return
        if not (Path(game) / "default.xex").is_file():
            messagebox.showerror("Locate", "That folder doesn't contain default.xex.")
            return
        self.show_settings(save_install(exe, game))

    def on_close(self):
        if isinstance(self.view, InstallView) and self.view.busy():
            if not messagebox.askyesno("Install running", "An install is still running. Quit anyway?\n"
                                       "Running the launcher again will pick up where it left off."):
                return
        self.root.destroy()

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    Launcher().run()
