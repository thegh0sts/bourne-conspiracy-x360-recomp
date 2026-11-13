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
import math
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import tbc_config as cfg
from tbc_installer import (ACCENT, APP_NAME, BG, DIM, FG, LAUNCHER_HOME, InstallView, install_info_path,
                           make_root, start_game)

VERSION = "0.2.3"
SETTINGS_PATH = LAUNCHER_HOME / "settings.json"
PANEL = "#1c2029"

# Renderer (--gpu_plugin). "native" is Skullburglar's Vulkan renderer (native-plugin/, from
# github.com/nlwstein/skullburglar, BSD 3-Clause): it translates the game's GPU commands into
# ordinary Vulkan drawing instead of emulating the Xbox 360's EDRAM, so higher render
# resolutions cost far less. It needs the game's 2x MSAA off (src/msaa.cpp, the default).
# "xenos" is ReXGlue's own Direct3D 12 renderer, kept as the fallback. The render resolution
# reaches the native renderer through the BOTS_GFX_RENDER_SCALE environment variable. The
# upscaling filter (--present_effect) works with both: it's the runtime's presenter, which
# scales the finished picture to the window for either renderer (FSR / CAS have Vulkan
# shaders too). The graphics path and FXAA are Xenos (Direct3D 12) settings.
RENDERERS = ["Native (Vulkan, recommended)", "Xenos (Direct3D 12)"]
RENDERER_PLUGIN = {RENDERERS[0]: "native", RENDERERS[1]: "xenos"}
RESOLUTIONS = ["Original (720p)", "1080p", "1440p", "4K"]
RES_FLAG = {"1080p": "1080p", "1440p": "1440p", "4K": "4k"}
RES_SIZE = {"1080p": (1920, 1080), "1440p": (2560, 1440), "4K": (3840, 2160)}
# Render resolution: the size the game actually draws at (the runtime's --resolution_scale
# multiplies the Xbox 360's 1280x720). Shown as resolutions rather than 1x/2x/3x.
# Scaling was expensive in this runtime: on an RTX 4080 Super, one heavy street scene ran
# about 48 FPS at 1x, about 20 at 2x and under 10 at 3x, with the GPU near full load. Most of
# that was EDRAM copies; the ROV graphics path (RENDER_PATHS below) avoids them.
# 1080p is 1.5x: the native renderer takes any scale from 0.25 to 3 (BOTS_GFX_RENDER_SCALE), but
# Xenos only whole numbers (--resolution_scale), so there it draws at 2x (1440p).
SCALES = ["1280×720 (original)", "1920×1080 (Xenos: 1440p)", "2560×1440 (demanding)",
          "3840×2160 (very demanding)"]
SCALE_FACTOR = {SCALES[0]: 1, SCALES[1]: 1.5, SCALES[2]: 2, SCALES[3]: 3}
OLD_SCALE_NAMES = {"1x (original)": SCALES[0], "2x": SCALES[2], "3x": SCALES[3],
                   "2560×1440": SCALES[2], "3840×2160": SCALES[3]}
# Output filter, applied by the runtime to the finished picture (--present_effect). FSR and
# CAS exist only in an SDK built with AMD FidelityFX (tools/build_sdk.bat); the stock SDK
# ignores the setting and keeps bilinear. They also treat the game's menus and HUD as part of
# the picture (the game draws them into the same image), so text gets softer. FSR is still the
# default: with the native renderer at the original render resolution on a 1440p or 4K screen,
# bilinear looked very pixelated next to it.
# How the runtime imitates the Xbox 360's 10 MB of on-chip framebuffer memory (EDRAM),
# --render_target_path_d3d12. "rtv" (the runtime's default on NVIDIA/AMD) gives each image the
# game draws its own render target and copies pixels between them whenever the game reuses a
# piece of EDRAM for another image; Bourne does that ~1,800 times a frame. A PIX capture of the
# first street (RTX 4080 Super) put those copies at 12 ms of a 22.5 ms frame at 1x and 54 of 68
# ms at 2x, since each has 4x the pixels. "rov" keeps EDRAM as one buffer and blends in the
# pixel shaders (rasterizer-ordered views), so there are no copies: 2x went from the mid-teens
# to the mid-40s FPS. Auto uses rov above the original resolution.
RENDER_PATHS = ["Auto (ROV above 720p)", "Render targets (RTV)", "Shader EDRAM (ROV)"]
RENDER_PATH_FLAG = {RENDER_PATHS[1]: "rtv", RENDER_PATHS[2]: "rov"}
UPSCALE = ["Bilinear (original)", "FSR (sharper 3D, softer text)", "CAS (sharpen only)"]
UPSCALE_FLAG = {UPSCALE[1]: "fsr", UPSCALE[2]: "cas"}
AA = ["Off", "FXAA", "FXAA (strong)"]
AA_FLAG = {"FXAA": "fxaa", "FXAA (strong)": "fxaa_extreme"}
# Frame rate. tbc.exe's --frame_rate (src/fps_unlock.cpp) sets both the game's present
# interval and a host frame limiter. The limiter does the pacing because the launcher turns
# the graphics plugin's guest vsync off (--vsync=false): with it on, the plugin sleeps on
# every GPU wait, which cost a lot of frame rate (the Table Tennis PC port measured 30-48
# FPS instead of 60). Above 60, the engine's own limiter (MaxSmoothedFrameRate, 62 by
# default) is raised out of the way.
FPS = ["30 (original)", "60", "120 (experimental)", "144 (experimental)", "Unlimited (experimental)"]
FPS_MODE = {  # label: (--frame_rate, MaxSmoothedFrameRate or None to keep the original 62)
    "30 (original)": (30, None),
    "60": (60, None),
    "120 (experimental)": (120, "1000"),
    "144 (experimental)": (144, "1000"),
    "Unlimited (experimental)": (0, "1000"),
}
# Logo movies played before the menu (BourneGame\Movies). An earlier "Skip intro logos"
# option renamed them so the game couldn't find them, but that left the game stuck on the
# first level's loading screen, so the option is gone. The launcher still renames them back
# on Play for anyone who had it ticked.
INTRO_MOVIES = ["THXLogo51Mix.bik", "SierraHighMoonLogos.bik"]
SKIPPED_SUFFIX = ".skipped"

# Keyboard and mouse: the SDK's driver that acts as a virtual Xbox controller (--mnk_mode),
# with the mouse on the right stick (--mnk_mouse). It merges with a real controller on
# player 1. Every bind is passed, so none of the SDK's own defaults (Semicolon for A, Shift+arrows
# for the d-pad...) clash with this layout. Key names are the SDK's (src/ui/keybinds.cpp);
# a comma separates alternatives.
KBM_BINDS = [  # (setting, default keys, action in Bourne, from Xe-BourneInput.ini)
    ("keybind_lstick_up", "W", "Move forward"),
    ("keybind_lstick_down", "S", "Move back"),
    ("keybind_lstick_left", "A", "Move left"),
    ("keybind_lstick_right", "D", "Move right"),
    ("keybind_right_trigger", "LMB", "Fire"),
    ("keybind_left_trigger", "RMB", "Aim / targeting"),
    ("keybind_a", "Space,Return", "Action / block; accept in menus"),
    ("keybind_b", "F,Backspace", "Takedown / skip cutscene; back in menus"),
    ("keybind_x", "E,R", "Light attack / reload"),
    ("keybind_y", "Q", "Heavy attack / Bourne Instinct"),
    ("keybind_right_shoulder", "Shift", "Sprint"),
    ("keybind_left_shoulder", "G", "Weapon"),
    ("keybind_lstick_press", "Control", "Crouch"),
    ("keybind_rstick_press", "MMB", "Recentre camera / switch shoulder"),
    ("keybind_start", "Escape", "Pause"),
    ("keybind_back", "Tab", "Back"),
    ("keybind_rstick_up", "Up", "Camera up (as well as the mouse)"),
    ("keybind_rstick_down", "Down", "Camera down"),
    ("keybind_rstick_left", "Left", "Camera left"),
    ("keybind_rstick_right", "Right", "Camera right"),
    ("keybind_dpad_up", "1", "D-pad up"),
    ("keybind_dpad_down", "2", "D-pad down"),
    ("keybind_dpad_left", "3", "D-pad left"),
    ("keybind_dpad_right", "4", "D-pad right"),
]
KBM_DEFAULT = {name: keys for name, keys, _ in KBM_BINDS}
# The layout is fixed: tbc.exe uses these binds as they are and never changes them while the
# game runs, so every key can be rebound. The prompts show the key bound to each button, and in
# menus Enter / Backspace when A / B have them (keyboard_prompts.cpp). QTEs show the Xbox
# button in the 3D scene, so the editor names the button beside the face buttons' keys.
KBM_NOTES = {
    "keybind_a": "QTE: A", "keybind_b": "QTE: B", "keybind_x": "QTE: X", "keybind_y": "QTE: Y",
}
# Keys the runtime keeps for itself: debug overlay, console, settings, achievements.
RESERVED_KEYS = {"F3": "the debug overlay", "Backtick": "the console", "F4": "the settings overlay",
                 "F7": "the achievements overlay"}
KEY_NAMES = {"LMB": "Left mouse", "RMB": "Right mouse", "MMB": "Middle mouse", "Return": "Enter",
             "Escape": "Esc", "Control": "Ctrl", "Backtick": "`", "LBracket": "[", "RBracket": "]",
             "Quote": "'", "Semicolon": ";", "Comma": ",", "Period": ".", "Slash": "/",
             "Backslash": "\\", "Minus": "-", "Plus": "="}
# Tk key names (keysyms) to the SDK's key names (src/ui/keybinds.cpp).
TK_KEYS = {"space": "Space", "Return": "Return", "Escape": "Escape", "Tab": "Tab", "BackSpace": "Backspace",
           "Delete": "Delete", "Insert": "Insert", "Home": "Home", "End": "End", "Prior": "PageUp",
           "Next": "PageDown", "Left": "Left", "Right": "Right", "Up": "Up", "Down": "Down",
           "Shift_L": "Shift", "Shift_R": "Shift", "Control_L": "Control", "Control_R": "Control",
           "Alt_L": "Alt", "Alt_R": "Alt", "grave": "Backtick", "minus": "Minus", "equal": "Plus",
           "comma": "Comma", "period": "Period", "semicolon": "Semicolon", "slash": "Slash",
           "backslash": "Backslash", "bracketleft": "LBracket", "bracketright": "RBracket",
           "apostrophe": "Quote", "KP_Enter": "NumpadEnter", "KP_Add": "NumpadPlus",
           "KP_Subtract": "NumpadMinus", "KP_Multiply": "NumpadStar", "KP_Divide": "NumpadSlash",
           "Caps_Lock": "CapsLock", "Num_Lock": "NumLock", "Scroll_Lock": "ScrollLock", "Pause": "Pause",
           "Print": "PrintScreen"}


def tk_key_name(keysym):
    """SDK key name for a Tk keysym, or None if the SDK can't bind it."""
    if keysym in TK_KEYS:
        return TK_KEYS[keysym]
    if len(keysym) == 1 and keysym.isascii() and keysym.isalnum():
        return keysym.upper()
    if keysym.startswith("KP_") and keysym[3:].isdigit():
        return "Numpad" + keysym[3:]
    if keysym[:1] == "F" and keysym[1:].isdigit() and 1 <= int(keysym[1:]) <= 24:
        return keysym
    return None


def key_label(keys):
    return " or ".join(KEY_NAMES.get(k, k) for k in keys.split(",") if k) or "(none)"


def kbm_binds(s):
    """The player's key binds, filled in from the defaults."""
    binds = dict(KBM_DEFAULT)
    saved = s.get("kbm_binds") or {}
    binds.update({k: v for k, v in saved.items() if k in KBM_DEFAULT and isinstance(v, str)})
    # From the earlier layout, where Enter was on Start (and moved to A by tbc.exe in menus):
    # Enter now goes on A and Backspace on B, unless the player put them elsewhere.
    if binds.get("keybind_start") == "Escape,Return":
        binds["keybind_start"] = "Escape"
    for name, key in (("keybind_a", "Return"), ("keybind_b", "Backspace")):
        keys = [k for k in binds[name].split(",") if k]
        if len(keys) < 2 and not any(key in v.split(",") for v in binds.values()):
            binds[name] = ",".join(keys + [key])
    return binds


def kbm_layout_text(binds):
    return "\n".join(f"{key_label(binds[name])}: {what}" for name, _, what in KBM_BINDS)


class KeyBindDialog(tk.Toplevel):
    """Rebind the keyboard-and-mouse controls: two keys per action."""

    def __init__(self, master, binds, on_save):
        super().__init__(master, bg=BG)
        self.title("Keyboard and mouse controls")
        self.transient(master)
        self.binds = dict(binds)
        self.on_save = on_save
        self.buttons = {}
        self.waiting = None  # (setting, slot) while waiting for a key
        self.note = tk.StringVar(value="Click a key, then press the new key or click the new mouse button. "
                                       "The mouse always moves the camera. QTEs show Xbox buttons: the keys of "
                                       "A, B, X and Y answer them.")
        tk.Label(self, textvariable=self.note, font=("Segoe UI", 9), fg=DIM, bg=BG, wraplength=520,
                 justify="left").pack(anchor="w", padx=16, pady=(12, 6))
        grid = tk.Frame(self, bg=PANEL, padx=12, pady=8)
        grid.pack(fill="both", expand=True, padx=16)
        for row, (name, _, what) in enumerate(KBM_BINDS):
            tk.Label(grid, text=what, font=("Segoe UI", 9), fg=FG, bg=PANEL,
                     anchor="w").grid(row=row, column=0, sticky="w", padx=(0, 12))
            for slot in (0, 1):
                b = tk.Button(grid, width=14, font=("Segoe UI", 9), bd=0, relief="flat",
                              command=lambda n=name, sl=slot: self.start(n, sl))
                b.grid(row=row, column=1 + slot, padx=2, pady=1)
                self.buttons[(name, slot)] = b
            if name in KBM_NOTES:
                tk.Label(grid, text=KBM_NOTES[name], font=("Segoe UI", 8), fg=DIM, bg=PANEL,
                         anchor="w").grid(row=row, column=3, sticky="w", padx=(6, 0))
        foot = tk.Frame(self, bg=BG)
        foot.pack(fill="x", padx=16, pady=12)
        tk.Button(foot, text="Save", width=10, command=self.save, bg=ACCENT, fg="white", bd=0,
                  font=("Segoe UI", 10, "bold")).pack(side="right")
        tk.Button(foot, text="Cancel", width=10, command=self.destroy, bd=0,
                  font=("Segoe UI", 10)).pack(side="right", padx=8)
        tk.Button(foot, text="Defaults", width=10, command=self.defaults, bd=0,
                  font=("Segoe UI", 10)).pack(side="left")
        tk.Button(foot, text="Clear key", width=10, command=self.clear, bd=0,
                  font=("Segoe UI", 10)).pack(side="left", padx=8)
        self.bind("<KeyPress>", self.on_key)
        self.refresh()
        self.grab_set()
        self.focus_set()

    def keys_of(self, name):
        return [k for k in self.binds[name].split(",") if k]

    def refresh(self):
        for (name, slot), b in self.buttons.items():
            keys = self.keys_of(name)
            waiting = self.waiting == (name, slot)
            b.config(text="Press a key..." if waiting else (KEY_NAMES.get(keys[slot], keys[slot])
                                                            if slot < len(keys) else "-"),
                     bg=ACCENT if waiting else "#2a2f38", fg="white" if waiting else FG,
                     activebackground=ACCENT)

    def start(self, name, slot):
        self.unbind_mouse()
        self.waiting = (name, slot)
        self.note.set("Press the new key, or click this button with the mouse button you want "
                      "(left, right or middle). Clear key removes it.")
        b = self.buttons[(name, slot)]
        # Mouse buttons are caught on the button itself: 1 left, 2 middle, 3 right.
        for n, key in (("1", "LMB"), ("2", "MMB"), ("3", "RMB")):
            b.bind(f"<ButtonRelease-{n}>", lambda e, k=key: self.assign(k), add=False)
        self.after(1, self.refresh)
        self.focus_set()

    def on_key(self, event):
        if not self.waiting:
            return
        key = tk_key_name(event.keysym)
        if key is None:
            self.note.set(f"That key ({event.keysym}) can't be bound. Try another.")
            return
        self.assign(key)
        return "break"

    def assign(self, key):
        if not self.waiting:
            return
        if key in RESERVED_KEYS:
            self.note.set(f"{KEY_NAMES.get(key, key)} is kept for {RESERVED_KEYS[key]}. Choose another key.")
            return "break"
        name, slot = self.waiting
        # A key does one thing: take it off any other action first.
        moved = []
        for other in self.binds:
            if other != name and key in self.keys_of(other):
                self.binds[other] = ",".join(k for k in self.keys_of(other) if k != key)
                moved.append(dict((n, w) for n, _, w in KBM_BINDS)[other])
        keys = [k for k in self.keys_of(name) if k != key]
        keys.insert(min(slot, len(keys)), key)
        self.binds[name] = ",".join(keys[:2])
        self.note.set(f"{KEY_NAMES.get(key, key)} moved here from: {', '.join(moved)}." if moved
                      else "Saved when you click Save. Click another key to change it.")
        self.stop()
        return "break"

    def unbind_mouse(self):
        if self.waiting:
            b = self.buttons[self.waiting]
            for n in ("1", "2", "3"):
                b.unbind(f"<ButtonRelease-{n}>")

    def stop(self):
        self.unbind_mouse()
        self.waiting = None
        self.refresh()

    def clear(self):
        if not self.waiting:
            self.note.set("Click the key you want to clear first, then Clear key.")
            return
        name, slot = self.waiting
        keys = self.keys_of(name)
        if slot < len(keys):
            del keys[slot]
        self.binds[name] = ",".join(keys)
        self.stop()

    def defaults(self):
        self.binds = dict(KBM_DEFAULT)
        self.stop()

    def save(self):
        self.on_save(self.binds)
        self.destroy()


DEFAULTS = {
    "renderer": RENDERERS[0],
    "fullscreen": False,
    "resolution": "Original (720p)",
    "render_scale": SCALES[0],
    "aa": "Off",
    "upscale": UPSCALE[1],
    "render_path": RENDER_PATHS[0],
    "vsync": True,
    "original_pacing": False,
    "show_fps": False,
    "fps": "30 (original)",
    "motion_blur": True,
    "depth_of_field": True,
    "bloom": True,
    "lens_flares": True,
    "distortion": True,
    "fov": 100,
    "deadzone": 25,
    "kbm": False,
    "mouse_attacks": True,
    "mouse_sensitivity": 100,
    "turn_speed": 100,
    "close_on_play": True,
}


def load_settings():
    s = dict(DEFAULTS)
    try:
        s.update(json.loads(SETTINGS_PATH.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        pass
    s["render_scale"] = OLD_SCALE_NAMES.get(s["render_scale"], s["render_scale"])
    if s.get("fps") not in FPS:  # labels from older launcher versions
        s["fps"] = DEFAULTS["fps"]
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


# Bourne's camera turn rates, one [<Mode>LookScales BnLookScales] section per camera mode
# (navigation, jogging, sprinting, crouching, over the shoulder...), in Xe-BourneGame.ini.
LOOK_RATE_KEYS = ("LookRightScale", "LookUpScale", "RawJoyLookRightModifier")


def _edit_look_scales(body, factor, linear):
    """Scale every mode's turn rates by `factor`; with `linear`, drop the aiming slow zone
    (over the shoulder: 30% turn speed until the stick is past 95%, which a mouse rarely reaches)."""
    lines = body.split("\r\n")
    in_look = False
    for i, line in enumerate(lines):
        if line.startswith("["):
            in_look = "LookScales" in line
            continue
        if not in_look or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in LOOK_RATE_KEYS and factor != 1:
            try:
                lines[i] = f"{key}={float(value) * factor:.6f}"
            except ValueError:
                pass
        elif key == "BelowStepMultiplier" and linear:
            lines[i] = f"{key}=1.000000"
    return "\r\n".join(lines)


# Hand-to-hand fights on the mouse buttons. Bourne's own input config (Xe-BourneInput.ini,
# [Engine.PlayerInput]; the last binding of a button wins) has X = "MeleeAttackLight | ...",
# Y = "... MeleeAttackHeavy ...", RT = fire, LT = aim. The game only carries out the melee
# commands in a fight and the gun ones with a gun out, so adding the attacks to the triggers
# (left / right mouse) makes the mouse buttons attack in fights and shoot outside them. The
# attacks also come off X and Y, so in fights only the mouse buttons attack. This is the
# game's input config, so with the option on a controller's X / Y don't attack either: its
# triggers do.
FIGHT_MOUSE_EDITS = [
    ('Bindings=(Name="XboxTypeS_RightTrigger",Command="StartFire | OnRelease StopFire | Axis aBaseZ Speed=+1.0")',
     'Bindings=(Name="XboxTypeS_RightTrigger",Command="StartFire | OnRelease StopFire | MeleeAttackLight | '
     'OnRelease MeleeReleaseLight | Axis aBaseZ Speed=+1.0")'),
    ('Bindings=(Name="XboxTypeS_LeftTrigger",Command="EnterWallworkShooting |OnRelease ExitWallworkShooting | Axis aBaseZ Speed=-1.0")',
     'Bindings=(Name="XboxTypeS_LeftTrigger",Command="EnterWallworkShooting |OnRelease ExitWallworkShooting | '
     'MeleeAttackHeavy | OnRelease MeleeReleaseHeavy | Axis aBaseZ Speed=-1.0")'),
    # ...and off X and Y, so in fights E / Q (X / Y) no longer attack; outside fights they keep
    # reload (X) and Bourne Instinct (Y).
    ('Bindings=(Name="XboxTypeS_X",Command="MeleeAttackLight | OnRelease MeleeReleaseLight | ReloadWeapon")',
     'Bindings=(Name="XboxTypeS_X",Command="ReloadWeapon")'),
    ('Bindings=(Name="XboxTypeS_Y",Command="StartAwareness | OnRelease StopAwareness | MeleeAttackHeavy | OnRelease MeleeReleaseHeavy")',
     'Bindings=(Name="XboxTypeS_Y",Command="StartAwareness | OnRelease StopAwareness")'),
]


def _edit_fight_mouse(body):
    for old, new in FIGHT_MOUSE_EDITS:
        body = body.replace(old, new, 1)
    return body


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

    edits = []  # (file, section, key, value)
    _, cap = FPS_MODE[s["fps"]]
    if cap:
        edits.append(("Xe-BourneEngine.ini", "Engine.GameEngine", "MaxSmoothedFrameRate", cap))
    if not s["motion_blur"]:
        edits.append(("Xe-BourneEngine.ini", "SystemSettings", "MotionBlur", "False"))
    if not s["depth_of_field"]:
        edits.append(("Xe-BourneEngine.ini", "SystemSettings", "DepthOfField", "False"))
    if not s["bloom"]:
        edits.append(("Xe-BourneEngine.ini", "SystemSettings", "Bloom", "False"))
    if not s["lens_flares"]:
        edits.append(("Xe-BourneEngine.ini", "SystemSettings", "LensFlares", "False"))
    if not s["distortion"]:
        edits.append(("Xe-BourneEngine.ini", "SystemSettings", "Distortion", "False"))
    if int(s["deadzone"]) != 25:
        edits.append(("Xe-BourneInput.ini", "HM_Engine.HmPlayerInput", "DeadZone", f"{int(s['deadzone']) / 100:.2f}"))
    fov = int(s["fov"])
    turn = int(s.get("turn_speed", 100))
    linear_aim = bool(s.get("kbm"))  # the mouse gets the full turn speed while aiming
    look_edit = turn != 100 or linear_aim
    fight_mouse = bool(s.get("kbm")) and bool(s.get("mouse_attacks", True))

    if turn != 100:
        # The camera's top yaw rate (100 in the original): a cap on how fast it can turn,
        # whatever the mode's turn rate, so it scales with the turn speed too.
        edits.append(("Xe-BourneInput.ini", "BourneNative.BnPlayerInput", "MaxYawRate", f"{100 * turn / 100:g}"))

    if not edits and fov == 100 and not look_edit and not fight_mouse:
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
    if look_edit:
        i = cfg.find_file(files, "Xe-BourneGame.ini")
        name, body = files[i]
        files[i] = (name, _edit_look_scales(body, turn / 100, linear_aim))
    if fight_mouse:
        i = cfg.find_file(files, "Xe-BourneInput.ini")
        name, body = files[i]
        files[i] = (name, _edit_fight_mouse(body))
    shutil.copy2(toc_bak, toc)  # update the TOC entry starting from the original
    cfg.write(game, files)
    return f"{len(edits) + (fov != 100) + look_edit + fight_mouse} config change(s)"


def apply_intro_skip(game: Path, skip: bool) -> None:
    """Rename the intro logo movies away (skip) or back (don't skip)."""
    movies = game / "BourneGame" / "Movies"
    for name in INTRO_MOVIES:
        on, off = movies / name, movies / (name + SKIPPED_SUFFIX)
        if skip and on.exists() and not off.exists():
            on.rename(off)
        elif not skip and off.exists() and not on.exists():
            off.rename(on)


def logs_dir(info) -> Path:
    return Path(info.get("root") or Path(info["exe"]).parent)


def save_crash_report(info) -> Path:
    """Zip the game log, the newest crash files and the launcher settings onto the desktop."""
    root = logs_dir(info)
    exe_logs = Path(info["exe"]).parent / "logs"
    files = []
    for p in (root / "game.log", SETTINGS_PATH, install_info_path()):
        if p.is_file():
            files.append(p)
    crash = sorted(exe_logs.glob("crash-*.*"), key=lambda p: p.stat().st_mtime, reverse=True) if exe_logs.is_dir() else []
    if crash:
        stem = crash[0].stem  # crash-<pid>: the newest crash's .txt and .dmp
        files += [p for p in crash if p.stem == stem]
    desktop = Path(os.environ.get("USERPROFILE", str(Path.home()))) / "Desktop"
    if not desktop.is_dir():
        desktop = Path.home()
    out = desktop / f"BourneConspiracy-report-{time.strftime('%Y%m%d-%H%M%S')}.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for p in files:
            z.write(p, p.name)
    return out


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
    args = ["--gpu_plugin", RENDERER_PLUGIN.get(s.get("renderer"), "native")]
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
    # Graphics-plugin settings are always passed, so a value left in tbc.toml (the runtime's
    # own config file, written by its F4 settings overlay) can't override the launcher.
    scale = math.ceil(SCALE_FACTOR.get(s["render_scale"], 1))  # Xenos: whole numbers only
    args += ["--resolution_scale", str(scale)]
    path = RENDER_PATH_FLAG.get(s.get("render_path"), "rov" if scale > 1 else None)
    if path:
        args += ["--render_target_path_d3d12", path]
    args += ["--swap_post_effect", AA_FLAG.get(s["aa"], "none")]
    if s.get("upscale") in UPSCALE_FLAG:
        args += ["--present_effect", UPSCALE_FLAG[s["upscale"]]]
    # Guest vsync (the plugin's imitation of the Xbox 360 vblank): off, with tbc.exe's own
    # frame limiter holding the frame rate, unless "original frame pacing" is ticked.
    args.append("--vsync=true" if s.get("original_pacing") else "--vsync=false")
    # Display VSync: unticked lets presents tear instead of waiting for the display, which
    # also stops a present from stalling the graphics queue the emulation uses.
    args.append("--d3d12_allow_variable_refresh_rate_and_tearing=" + ("false" if s["vsync"] else "true"))
    rate, _ = FPS_MODE[s["fps"]]
    args.append(f"--frame_rate={rate}")
    if s.get("show_fps"):
        args.append("--show_fps=true")
    if s.get("kbm"):
        args += ["--mnk_mode=true", "--mnk_mouse=true",
                 "--mnk_sensitivity", f"{int(s.get('mouse_sensitivity', 100)) / 100:.2f}",
                 # Mouse movement starts just past the game's stick dead zone (the same value
                 # written to Xe-BourneInput.ini above), so slow aiming isn't swallowed by it.
                 # Needs SDK patch 0004; a stock SDK ignores it.
                 "--mnk_mouse_deadzone", f"{int(s['deadzone']) / 100:.2f}"]
        if s.get("mouse_attacks", True):
            # Fight prompts show light / heavy attack as [LMB] / [RMB] (keyboard_prompts.cpp),
            # to match the trigger bindings written to Xe-BourneInput.ini above.
            args.append("--keyboard_mouse_attacks=true")
        for name, keys in kbm_binds(s).items():
            if keys:  # an empty value would leave the SDK's own default for that button
                args.append(f"--{name}={keys}")
            else:
                args.append(f"--{name}=F24")  # unbound: park it on a key keyboards don't have
    return args


def game_env(s):
    """Environment settings for the game: the native renderer takes its render resolution
    from BOTS_GFX_RENDER_SCALE (it ignores --resolution_scale)."""
    env = {}
    if RENDERER_PLUGIN.get(s.get("renderer"), "native") == "native":
        env["BOTS_GFX_RENDER_SCALE"] = f'{SCALE_FACTOR.get(s["render_scale"], 1):g}'
    return env


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
        self._link(links, "Open logs", self.open_logs).pack(side="left")
        self._link(links, "Save crash report", self.crash_report).pack(side="left", padx=14)

        box = self._section("Display")
        self._combo(box, 0, "renderer", "Renderer", RENDERERS)
        self._check(box, 1, "fullscreen", "Fullscreen")
        self._combo(box, 2, "resolution", "Output resolution", RESOLUTIONS)
        self._combo(box, 3, "render_scale", "Render resolution (sharper, slower)", SCALES)
        self._combo(box, 4, "render_path", "Graphics path (Xenos only)", RENDER_PATHS)
        self._combo(box, 5, "upscale", "Upscaling filter", UPSCALE)
        self._combo(box, 6, "aa", "Anti-aliasing (Xenos only)", AA)
        self._combo(box, 7, "fps", "Frame rate", FPS)
        self._checks(box, 8, [("vsync", "VSync (off: may tear, can be faster)"), ("show_fps", "Show FPS counter")])
        self._check(box, 9, "original_pacing", "Original Xbox 360 frame pacing (slower)")
        tk.Label(box, text="Native renderer by the Skullburglar project (github.com/nlwstein/skullburglar, "
                           "BSD 3-Clause).", font=("Segoe UI", 8), fg=DIM, bg=PANEL, anchor="w"
                 ).grid(row=10, column=0, columnspan=2, sticky="w", pady=(4, 0))

        box = self._section("Graphics and camera")
        self._checks(box, 0, [("motion_blur", "Motion blur"), ("depth_of_field", "Depth of field"),
                              ("bloom", "Bloom")])
        self._checks(box, 1, [("lens_flares", "Lens flares"), ("distortion", "Heat haze / distortion")])
        self._slider(box, 2, "fov", "Field of view (%)", 80, 140, 5)

        box = self._section("Controls")
        self._slider(box, 0, "deadzone", "Stick dead zone (%)", 5, 40, 1)
        line = tk.Frame(box, bg=PANEL)
        line.grid(row=1, column=0, columnspan=2, sticky="w", pady=(4, 0))
        v = self.vars["kbm"] = tk.BooleanVar(value=self.s["kbm"])
        tk.Checkbutton(line, text="Keyboard and mouse", variable=v, font=("Segoe UI", 10), fg=FG, bg=PANEL,
                       selectcolor=PANEL, activebackground=PANEL, activeforeground=FG).pack(side="left")
        self._link(line, "Edit keys...", self.edit_keys).pack(side="left", padx=14)
        self._slider(box, 2, "mouse_sensitivity", "Mouse sensitivity (%)", 10, 1000, 10)
        self._slider(box, 3, "turn_speed", "Camera turn speed (%)", 50, 300, 10)
        self._check(box, 4, "mouse_attacks", "Fights: left / right mouse = light / heavy attack")

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

    def _checks(self, box, row, items):
        """Several checkboxes side by side on one row."""
        line = tk.Frame(box, bg=PANEL)
        line.grid(row=row, column=0, columnspan=2, sticky="w")
        for key, text in items:
            v = self.vars[key] = tk.BooleanVar(value=self.s[key])
            tk.Checkbutton(line, text=text, variable=v, font=("Segoe UI", 10), fg=FG, bg=PANEL, selectcolor=PANEL,
                           activebackground=PANEL, activeforeground=FG).pack(side="left", padx=(0, 18))

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
        self.s["kbm_binds"] = {}  # key binds back to the default layout too

    def edit_keys(self):
        def saved(binds):
            changed = {k: v for k, v in binds.items() if v != KBM_DEFAULT[k]}
            self.s["kbm_binds"] = changed
            save_settings(self.collect())
            self.status.set("Key binds saved." if changed else "Key binds back to defaults.")
        KeyBindDialog(self.winfo_toplevel(), kbm_binds(self.s), saved)

    def open_logs(self):
        folder = logs_dir(self.info)
        try:
            os.startfile(str(folder))
        except OSError as e:
            messagebox.showerror("Open logs", f"Couldn't open {folder}: {e}")

    def crash_report(self):
        try:
            out = save_crash_report(self.info)
        except OSError as e:
            messagebox.showerror("Crash report", f"Couldn't save the report: {e}")
            return
        messagebox.showinfo("Crash report", f"Saved {out.name} to {out.parent}.\n\n"
                                            "Attach it when you report a problem. It contains the game log, "
                                            "your launcher settings and any crash dump (no game files).")

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
        try:
            apply_intro_skip(game, False)  # undo the removed "skip intro" option, if it was used
        except OSError as e:
            messagebox.showwarning("Play", f"Couldn't change the intro movies ({e}). Starting anyway.")
        log = Path(self.info.get("root", exe.parent)) / "game.log"
        args = [str(exe), "--game_data_root", str(game)] + game_args(s, (self.winfo_screenwidth(), self.winfo_screenheight())) + ["--log_file", str(log)]
        try:
            if RENDERER_PLUGIN.get(s.get("renderer")) == "native" and not any(exe.parent.glob("rexgpu-native*.dll")):
                messagebox.showwarning("Play", "The native renderer isn't installed (this build's SDK "
                                               "wasn't made with it). Starting with Xenos instead.")
                args[args.index("native")] = "xenos"
            start_game(args, exe.parent, game_env(s))
        except OSError as e:
            messagebox.showerror("Play", f"Couldn't start the game: {e}")
            return
        self.status.set(f"Started with {summary}.")
        if s["close_on_play"]:
            self.after(800, self.winfo_toplevel().destroy)


class Launcher:
    def __init__(self):
        self.root = make_root(APP_NAME)
        height = min(980, self.root.winfo_screenheight() - 80)
        self.root.geometry(f"780x{height}")
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
