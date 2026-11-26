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
import threading
import time
import urllib.request
import webbrowser
import zipfile
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import launcher_text
import tbc_config as cfg
import tbc_reticle
import xex_achievements
from launcher_text import tr, tr_label
from tbc_installer import (ACCENT, APP_NAME, BG, DIM, EXE_NAME, FG, IS_WINDOWS, LAUNCHER_HOME, InstallView,
                           data_home, desktop_dir, install_info_path, make_root, open_path, start_game)

VERSION = "0.3.6"
# Update check: the newest release on GitHub, asked once when the launcher opens.
RELEASES_API = "https://api.github.com/repos/thegh0sts/bourne-conspiracy-x360-recomp/releases/latest"
RELEASES_URL = "https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/releases/latest"
# The keyboard-and-mouse layout, explained (the "Key binds" window's full-page link).
CONTROLS_URL = "https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/docs/keyboard-and-mouse.md"
SETTINGS_PATH = LAUNCHER_HOME / "settings.json"
PANEL = "#1c2029"
# The settings' controls (button groups, drop-downs, buttons), in this launcher's colours.
ACCENT_HOVER = "#e0203f"
ON_ACCENT = "white"
FIELD_BG, FIELD_HOVER, BORDER = "#2a2f38", "#353b46", "#4a505a"
# The last launcher version whose changes need the game rebuilt (src/, the SDK, the build). A
# launcher-only release leaves this alone, so games built with it aren't asked to reinstall.
GAME_VERSION = "0.3.5"
# The FPS counter: off, the frame rate, or the frame rate with frame time, GPU, CPU and memory
# (--show_fps_detail; the native renderer's counter only, Xenos shows the frame rate).
# The Controls tab's input device: a controller (Xbox or PlayStation, its buttons shown to match), or keyboard and
# mouse (controllers off: --input_backend none). Saved as the choice, with "kbm" following it.
INPUT_DEVICES = ["Controller", "Keyboard and mouse"]
FPS_COUNTER = ["Off", "FPS", "Detailed (GPU, CPU, memory)"]
# The game's languages (the disc has these five): the runtime's user_language, an XLanguage id.
LANGUAGES = {"English": 1, "German": 3, "French": 4, "Spanish": 5, "Italian": 6}

# The settings screen's tabs, top to bottom.
TABS = ("Game files", "Display", "Graphics", "Camera", "Controls", "Saves", "Achievements", "Credits")

# Renderer (--gpu_plugin). "native" is Skullburglar's Vulkan renderer (native-plugin/, from
# github.com/nlwstein/skullburglar, BSD 3-Clause): it translates the game's GPU commands into
# ordinary Vulkan drawing instead of emulating the Xbox 360's EDRAM, so higher render
# resolutions cost far less. It needs the game's 2x MSAA off (src/msaa.cpp, the default).
# "xenos" is ReXGlue's own Direct3D 12 renderer, kept as the fallback. The render resolution
# reaches the native renderer through the BOTS_GFX_RENDER_SCALE environment variable. The
# upscaling filter (--present_effect) works with both: it's the runtime's presenter, which
# scales the finished picture to the window for either renderer (FSR / CAS have Vulkan
# shaders too). The graphics path is a Xenos (Direct3D 12) setting; FXAA works with both.
RENDERERS = ["Native (Vulkan, recommended)", "Xenos (Direct3D 12)"]
RENDERER_PLUGIN = {RENDERERS[0]: "native", RENDERERS[1]: "xenos"}
# Linux: the native renderer only (Xenos is the Direct3D 12 renderer here), and no FidelityFX filters (FSR / CAS
# are built for Direct3D 12 only), so the Display tab leaves those choices out.
RENDERER_CHOICES = RENDERERS if IS_WINDOWS else RENDERERS[:1]
# Output resolutions. The ultrawide ones (experimental, native renderer only) also set the
# picture's shape: the game draws a wider 720-line picture (--aspect_ratio, src/ultrawide.cpp)
# with a wider view, and the render resolutions offered are that shape too (render_scales), so
# an ultrawide screen never gets a 16:9 picture stretched or boxed into it, nor the reverse.
RESOLUTIONS = ["Original (720p)", "1080p", "1440p", "4K"]
ULTRAWIDE = ["2560×1080 ultrawide (experimental)", "3440×1440 ultrawide (experimental)",
             "3840×1600 ultrawide (experimental)"]
RES_FLAG = {"1080p": "1080p", "1440p": "1440p", "4K": "4k"}
RES_SIZE = {"1080p": (1920, 1080), "1440p": (2560, 1440), "4K": (3840, 2160),
            ULTRAWIDE[0]: (2560, 1080), ULTRAWIDE[1]: (3440, 1440), ULTRAWIDE[2]: (3840, 1600)}


def is_ultrawide(resolution):
    return resolution in ULTRAWIDE


def picture_width(resolution):
    """The game's 720-line picture width for an output resolution: 1280 for 16:9, wider for
    ultrawide (720 x the screen's shape, to 8 pixels, as src/ultrawide.cpp rounds it)."""
    if not is_ultrawide(resolution):
        return 1280
    w, h = RES_SIZE[resolution]
    return round(720 * w / h / 8) * 8
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


def render_scales(resolution):
    """Render resolutions offered for an output resolution: the 16:9 list for 16:9 outputs; for
    an ultrawide output, its own shape at 720, 1080, 1440 and 2160 lines plus its exact size."""
    if not is_ultrawide(resolution):
        return list(SCALES)
    pw = picture_width(resolution)
    w, h = RES_SIZE[resolution]
    out = []
    for k in sorted({1, 1.5, 2, 3, h / 720}):
        label = f"{round(pw * k)}×{round(720 * k)}"
        if k == 1:
            label += " (original height)"
        elif abs(k - h / 720) < 1e-6:
            label += " (your screen)"
        elif k == 3:
            label += " (very demanding)"
        out.append(label)
    return out


def scale_factor(label):
    """The render scale a render-resolution label stands for (its height / 720)."""
    if label in SCALE_FACTOR:
        return SCALE_FACTOR[label]
    try:
        return int(label.split("×")[1].split()[0]) / 720
    except (IndexError, ValueError):
        return 1


def matching_scale(resolution, label):
    """The render resolution for this output closest to an earlier choice (by render scale)."""
    options = render_scales(resolution)
    if label in options:
        return label
    want = scale_factor(label)
    return min(options, key=lambda o: abs(scale_factor(o) - want))
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
# Texture filtering (native renderer, BOTS_GFX_FORCE_ANISO, native-plugin/patches/0009): raises the
# game's anisotropic filtering to at least this on its surface textures (mipmapped, linearly
# filtered), so floors, roads and walls stay sharp at an angle. The HUD, fonts and lookup
# textures keep the game's own filtering. "Game's own" leaves every texture as the game set it.
TEXTURE_FILTERING = ["Game's own", "Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x"]
ANISOTROPY = {TEXTURE_FILTERING[1]: 4, TEXTURE_FILTERING[2]: 8, TEXTURE_FILTERING[3]: 16}
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
    ("keybind_lstick_up", "W", "Move forward; QTE top button"),
    ("keybind_lstick_down", "S", "Move back; QTE bottom button"),
    ("keybind_lstick_left", "A", "Move left; QTE left button"),
    ("keybind_lstick_right", "D", "Move right; QTE right button"),
    ("keybind_right_trigger", "LMB", "Fire; light attack in fights"),
    ("keybind_left_trigger", "RMB", "Aim / pop out of cover; heavy attack in fights"),
    ("keybind_a", "Space,Return", "Action / block; accept in menus"),
    ("keybind_b", "F,Backspace", "Takedown / skip cutscene; back in menus"),
    ("keybind_x", "E,R", "Reload"),
    ("keybind_y", "Q", "Bourne Instinct"),
    ("keybind_right_shoulder", "Shift", "Sprint"),
    ("keybind_left_shoulder", "G", "Weapon"),
    ("keybind_lstick_press", "Control", "Crouch"),
    ("keybind_rstick_press", "MMB", "Recentre camera / switch shoulder"),
    ("keybind_start", "Escape", "Pause"),
    ("keybind_back", "Tab", "Back"),
    # The right stick is the mouse only: the arrows press the d-pad (below), which moves through
    # menus and isn't used in gameplay. Empty binds are parked on F24 (none of the SDK's own
    # defaults, which put the arrows here).
    ("keybind_rstick_up", "", "Camera up (the mouse)"),
    ("keybind_rstick_down", "", "Camera down"),
    ("keybind_rstick_left", "", "Camera left"),
    ("keybind_rstick_right", "", "Camera right"),
    ("keybind_dpad_up", "1,Up", "D-pad up; menus"),
    ("keybind_dpad_down", "2,Down", "D-pad down; menus"),
    ("keybind_dpad_left", "3,Left", "D-pad left; menus"),
    ("keybind_dpad_right", "4,Right", "D-pad right; menus"),
]
KBM_DEFAULT = {name: keys for name, keys, _ in KBM_BINDS}
# The layout is fixed: the button prompts, the key glyphs drawn into the HUD's badges and QTE
# prompts (key_glyphs.cpp), the fight attacks on the mouse buttons and the QTEs on W / A / S / D
# are all set up around it, so it isn't editable. The launcher links to docs/keyboard-and-mouse.md.
# The keyboard-and-mouse layout as the "Key binds" window shows it (docs/keyboard-and-mouse.md
# has the same, with notes).
CONTROLS = [
    ("On foot", [
        ("W / A / S / D", "Move"),
        ("Scroll wheel", "Move speed: up faster, down slower"),
        ("Mouse", "Look around"),
        ("Left mouse", "Fire"),
        ("Right mouse", "Aim; in cover with a gun, pop out"),
        ("Shift", "Sprint"),
        ("Ctrl", "Crouch"),
        ("Space or Enter", "Action: doors, climb, vault, cover; block"),
        ("F or Backspace", "Takedown; skip cutscenes"),
        ("E or R", "Reload"),
        ("Q", "Bourne Instinct"),
        ("G", "Draw / holster the gun"),
        ("Middle mouse", "Recentre the camera; switch shoulder when aiming"),
        ("1 / 2 / 3 / 4", "D-pad up / down / left / right"),
        ("Esc", "Pause"),
        ("Tab", "Back"),
    ]),
    ("Fights", [
        ("Left mouse", "Light attack"),
        ("Right mouse", "Heavy attack"),
        ("Space", "Block"),
        ("F", "Takedown, once a tier of the adrenaline meter is full"),
    ]),
    ("Quick-time events: the button's place on the diamond", [
        ("W", "Top (Y)"),
        ("A", "Left (X)"),
        ("S", "Bottom (A)"),
        ("D", "Right (B)"),
    ]),
    ("Menus", [
        ("Enter or Space", "Accept; start on the title screen"),
        ("Backspace or F", "Back"),
        ("W / S or arrow keys", "Move up and down"),
    ]),
]


def version_key(tag):
    """'v0.2.4-1' -> (0, 2, 4, 1): numbers to compare; anything else in the tag counts as 0."""
    tag = tag.strip().lstrip("vV")
    main, _, build = tag.partition("-")
    nums = []
    for part in main.split("."):
        digits = "".join(c for c in part if c.isdigit())
        nums.append(int(digits) if digits else 0)
    nums += [0] * (3 - len(nums))
    digits = "".join(c for c in build if c.isdigit())
    return tuple(nums[:3]) + (int(digits) if digits else 0,)


# The launcher in a GitHub release: the Windows exe, or the Linux build (installer/build_installer.sh).
LAUNCHER_ASSET = "BourneConspiracy.exe" if IS_WINDOWS else "BourneConspiracy-linux-x86_64"


def latest_release(timeout=5):
    """(tag, page URL, BourneConspiracy.exe download URL or None) of the newest GitHub release,
    or None if it can't be had."""
    req = urllib.request.Request(RELEASES_API, headers={"Accept": "application/vnd.github+json",
                                                        "User-Agent": f"BourneConspiracyPC-launcher/{VERSION}"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = json.loads(r.read().decode("utf-8"))
        want = LAUNCHER_ASSET.lower()
        exe = next((a.get("browser_download_url") for a in data.get("assets", [])
                    if a.get("name", "").lower() == want), None)
        return data["tag_name"], data.get("html_url") or RELEASES_URL, exe
    except Exception:  # offline, rate-limited, blocked: no update notice, nothing else
        return None


def built_version(info):
    """The launcher version the installed game was built with; installs from before 0.2.5
    didn't record it."""
    return (info or {}).get("version") or "0.2.4-1"


def download_file(url, dest, progress, timeout=30):
    """Downloads url to dest (via a .part file), calling progress(fraction or None)."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": f"BourneConspiracyPC-launcher/{VERSION}"})
    with urllib.request.urlopen(req, timeout=timeout) as r, open(part, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            progress(done / total if total else None)
    part.replace(dest)


def kbm_binds(s):
    """The key binds: the fixed layout (binds saved by older launchers are ignored)."""
    return dict(KBM_DEFAULT)


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
    "fps_counter": "Off",
    "achievement_popups": True,
    "vibration_strength": 100,
    "language": "English",
    "brightness": 100,
    "texture_filtering": "Anisotropic 16x",
    # The native renderer records its Vulkan commands on a second thread (renderer patch 0022).
    "render_thread": True,
    "fps": "30 (original)",
    "motion_blur": True,
    "depth_of_field": True,
    "bloom": True,
    "lens_flares": True,
    "distortion": True,
    "fov": 100,
    "wide_pitch": True,
    "no_auto_recenter": False,
    "linear_stick": False,
    "deadzone": 25,
    # Controller: fight attacks on the triggers (RT light, LT heavy) instead of X / Y.
    "trigger_attacks": False,
    "kbm": False,  # follows input_device (collect, load_settings)
    "input_device": "Controller",
    "kbm_aim_assist": False,  # the game's sticky aim with keyboard and mouse on (always on without)
    "mouse_sensitivity": 100,
    "aim_sensitivity": 100,
    "mouse_look": True,
    "mouse_smoothing": 10,
    "mouse_look_direct": True,
    "turn_speed": 100,
    "stick_smoothing": 100,
    "custom_reticle": False,
    "reticle_size": 100,
    "close_on_play": True,
    # The launcher's own language (launcher_text.py): "same" as the game's, or en / de / fr / es / it.
    "launcher_language": "same",
}


def load_settings():
    s = dict(DEFAULTS)
    stored = {}
    try:
        stored = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
        s.update(stored)
    except (OSError, ValueError):
        pass
    s["render_scale"] = OLD_SCALE_NAMES.get(s["render_scale"], s["render_scale"])
    if stored.get("input_device") not in INPUT_DEVICES:  # 0.3.6 and earlier: a "Keyboard and mouse" tick box
        s["input_device"] = INPUT_DEVICES[1] if s.get("kbm") else INPUT_DEVICES[0]
    s["kbm"] = s["input_device"] == INPUT_DEVICES[1]
    s.pop("mouse_attacks", None)  # was an option; now always on with keyboard and mouse
    s["mouse_sensitivity"] = min(400, max(10, int(s.get("mouse_sensitivity", 100))))  # slider was 10-1000
    s.pop("aspect", None)  # the old separate "picture shape" setting: now set by the resolution
    if s.get("resolution") not in RESOLUTIONS + ULTRAWIDE:
        s["resolution"] = DEFAULTS["resolution"]
    if is_ultrawide(s["resolution"]) and RENDERER_PLUGIN.get(s.get("renderer")) != "native":
        s["resolution"] = DEFAULTS["resolution"]
    s["render_scale"] = matching_scale(s["resolution"], s["render_scale"])
    if s.get("fps_counter") not in FPS_COUNTER:  # 0.2.8 and earlier: a Show FPS counter checkbox
        s["fps_counter"] = "FPS" if s.get("show_fps") else "Off"
    s.pop("show_fps", None)
    if s.get("fps") not in FPS:  # labels from older launcher versions
        s["fps"] = DEFAULTS["fps"]
    if not IS_WINDOWS:
        s["renderer"] = RENDERERS[0]  # Linux: native only
        s["upscale"] = UPSCALE[0]     # no FidelityFX filters
    if s.pop("vibration", True) is False:  # earlier test builds: an on/off checkbox
        s["vibration_strength"] = 0
    s["vibration_strength"] = min(100, max(0, int(s.get("vibration_strength", 100))))
    if "hide_achievements" in s:  # 0.3.6 and earlier: a "Hide achievement pop-ups" checkbox
        s["achievement_popups"] = not s.pop("hide_achievements")
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
    """Remembers the install, and that this launcher's version built it ("version"): a newer
    launcher then knows the game needs rebuilding (SettingsView's rebuild notice)."""
    LAUNCHER_HOME.mkdir(parents=True, exist_ok=True)
    info = {"exe": str(exe), "game": str(game), "root": str(root or Path(exe).parent), "version": VERSION}
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
# triggers do. The same edits are the controller option "Fight attacks on the triggers": light
# attack on RT, heavy on LT (the prompts follow, --pad_trigger_attacks in keyboard_prompts.cpp).
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


# Mouse look (src/mouse_look.cpp): the camera turns by as much as the mouse moves. The game
# turns the camera through each strategy's orbit behaviours (BourneCamera.ini /
# Xe-BourneCamera.ini): a turn rate at full stick (YawSpeed / PitchSpeed, 65 to 120 degrees a
# second), a ramp that speeds it up after the stick is held for YawRampTime, and a smoother.
# For the mouse: full-stick rates 6x (so most flicks fit under full stick, and mouse_look.cpp
# carries the rest into the next frames; Play passes --mouse_look_full_rate=6 x 65), no ramp, and the smoothing cut to the "Mouse camera
# smoothing" setting (a percentage of the game's; 0.2.5 used 25%). The smoother's time never goes
# below 1% of the game's, so the behaviour keeps a non-zero time to work with.
MOUSE_RATE_FACTOR = 6  # 0.2.5-0.2.6: 4
MIN_SMOOTHING = 0.01


def _edit_camera_for_mouse(body, smoothing=0.25):
    lines = body.split("\r\n")
    kind = None
    for i, line in enumerate(lines):
        if line.startswith("["):
            kind = ("yaw" if "OrbitYawCameraBehavior" in line else
                    "pitch" if "OrbitPitch" in line else
                    "smoother" if "OrbitSmootherCameraBehavior" in line else None)
            continue
        if not kind or "=" not in line:
            continue
        key, value = line.split("=", 1)
        try:
            v = float(value)
        except ValueError:
            continue
        if kind == "yaw" and key == "YawSpeed":
            lines[i] = f"{key}={v * MOUSE_RATE_FACTOR:g}"
        elif kind == "yaw" and key == "YawRampTime":
            lines[i] = f"{key}=1000"
        elif kind == "pitch" and key == "PitchSpeed":
            lines[i] = f"{key}={v * MOUSE_RATE_FACTOR:g}"
        elif kind == "smoother" and key == "SmoothTime":
            lines[i] = f"{key}={v * max(smoothing, MIN_SMOOTHING):g}"
    return "\r\n".join(lines)


# Controller camera (the game's own copy of the config; the keyboard-and-mouse copy gets the mouse
# rates above instead). The look scales (_edit_look_scales) only shape the stick's travel: at full
# stick the camera turns at the orbit behaviour's YawSpeed / PitchSpeed (35 to 120 degrees a
# second), so up to 0.3.5 the stick turn speed made no difference once the stick was pushed all
# the way. Now the full-stick rates scale with it too.
# - turn: YawSpeed and PitchSpeed of every on-foot, cover, aiming and fight camera, times turn.
# - smoothing: the orbit smoother's time (0.2 s on foot, 0 aiming), as a share of the game's.
# - linear: no turn acceleration. The yaw behaviour adds YawRampSpeedModifier after the stick has
#   been held for YawRampTime (0.5 s; 0.8 s aiming); the ramp now takes 0.01 s, so the stick turns
#   at its top rate straight away. (0.3.5 and earlier set the ramp to 1000 s, which left the camera
#   at its slow starting rate for good.)
LINEAR_RAMP_TIME = 0.01


def _edit_camera_for_stick(body, turn=1.0, smoothing=1.0, linear=False):
    lines = body.split("\r\n")
    kind = None
    for i, line in enumerate(lines):
        if line.startswith("["):
            kind = ("yaw" if "OrbitYawCameraBehavior" in line else
                    "pitch" if "OrbitPitch" in line else
                    "smoother" if "OrbitSmootherCameraBehavior" in line else None)
            continue
        if not kind or "=" not in line:
            continue
        key, value = line.split("=", 1)
        try:
            v = float(value)
        except ValueError:
            continue
        if kind == "yaw" and key == "YawSpeed" and turn != 1:
            lines[i] = f"{key}={v * turn:g}"
        elif kind == "yaw" and key == "YawRampTime" and linear:
            lines[i] = f"{key}={LINEAR_RAMP_TIME:g}"
        elif kind == "pitch" and key == "PitchSpeed" and turn != 1:
            lines[i] = f"{key}={v * turn:g}"
        elif kind == "smoother" and key == "SmoothTime" and smoothing != 1:
            lines[i] = f"{key}={v * max(smoothing, MIN_SMOOTHING):g}"
    return "\r\n".join(lines)


def _edit_fight_mouse(body):
    for old, new in FIGHT_MOUSE_EDITS:
        body = body.replace(old, new, 1)
    return body


# Camera freedom (Xe-BourneCamera.ini / BourneCamera.ini, every camera mode's behaviours):
# - wide_pitch: on foot (chase, crouching, sprinting strategies' BnOrbitPitchSpeedDependentLimits
#   Idle / CreepWalk / Walk / Jog limits, about +-20 degrees; +20 / 0 while jogging) and on
#   stairs and in cover (HmOrbitPitchCameraBehavior PitchRange), the camera may look as far up
#   and down as when aiming over the shoulder (PitchRange -45..34 there): -45..35. The sprint
#   camera's fixed pitch and the fights' limits are left alone.
# - no_auto_recenter: no swing back behind Bourne: the idle recentre after 15 s
#   (HmOrbitIdleAutoRecenterCameraBehavior) and the yaw limits that pull the camera behind him
#   while walking, jogging and sprinting (BnOrbitYawSpeedDependentLimitsCameraBehavior: limit
#   angle, stick resistance, return speed). The manual recentre (middle mouse / right stick
#   press) still works.
# (linear_stick, no turn acceleration, is in _edit_camera_for_stick; with BelowStepMultiplier in
# the look scales (_edit_look_scales) it also takes away the slow zone while aiming.)
WIDE_PITCH = (-45.0, 35.0)


def _edit_camera_freedom(body, wide_pitch, no_auto_recenter):
    lines = body.split("\r\n")
    section = ""
    for i, line in enumerate(lines):
        if line.startswith("["):
            section = line
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        pitch = "OrbitPitch" in section and "Fighting" not in section
        if (wide_pitch and pitch and value.startswith("(Min=") and
                key in ("IdleLimits", "CreepWalkLimits", "WalkLimits", "JogLimits", "PitchRange")):
            m = re.match(r"\(Min=(-?[0-9.]+),Max=(-?[0-9.]+)\)(.*)$", value)
            if m:
                lo, hi = min(float(m.group(1)), WIDE_PITCH[0]), max(float(m.group(2)), WIDE_PITCH[1])
                lines[i] = f"{key}=(Min={lo:g},Max={hi:g}){m.group(3)}"
        elif no_auto_recenter and "IdleAutoRecenter" in section and key in ("DegreesPerSecond", "RecenterDelay"):
            lines[i] = f"{key}={'0' if key == 'DegreesPerSecond' else '100000'}"
        elif (no_auto_recenter and "YawSpeedDependentLimits" in section and "Fighting" not in section and
              key.endswith("Limits") and value.startswith("(")):
            value = re.sub(r"LimitAngle=[0-9.]+", "LimitAngle=181", value)
            value = re.sub(r"(Max|Min)StickResistance=[0-9.]+", r"\1StickResistance=0.0", value)
            value = re.sub(r"(Max|Min)ReturnSpeed=[0-9.]+", r"\1ReturnSpeed=0.0", value)
            lines[i] = f"{key}={value}"
    return "\r\n".join(lines)


# The keyboard-and-mouse copy of the config (Coalesced.ini), under the game folder. The game reads
# it instead of its own only when started with --mnk_mode=true (src/kbm_config.cpp, which mounts
# this folder in place of BourneGame\Config\Xenon\Cooked), so a run without the launcher's
# keyboard-and-mouse flags (Play.bat, a build started by hand) gets the controller config: up to
# 0.3.3 the keyboard-and-mouse changes went into the game's own file and stayed, leaving a
# controller's X / Y without attacks in fights. There is one Xbox360TOC.txt, which lists
# Coalesced.ini's size, so with keyboard and mouse ticked both copies are padded to one size.
KBM_CONFIG_DIR = Path("BourneGame") / "KeyboardMouse"


def apply_game_config(game: Path, s) -> str:
    """Rewrite Coalesced.ini from the original with settings applied, and with keyboard and mouse
    ticked, its keyboard-and-mouse copy too. Returns a short summary."""
    conf = game / cfg.CONFIG_REL
    toc = game / cfg.TOC_REL
    conf_bak = conf.with_name(conf.name + ".bak")
    toc_bak = toc.with_name(toc.name + ".bak")
    if not conf_bak.exists():
        shutil.copy2(conf, conf_bak)
    if not toc_bak.exists():
        shutil.copy2(toc, toc_bak)

    files, n = _build_game_config(conf_bak, s, kbm=False)
    summary = tr("{n} config change(s)", n=n) if files is not None else tr("original game config")
    kbm_dir = game / KBM_CONFIG_DIR
    if s.get("kbm"):
        if files is None:
            files = cfg.decode(conf_bak.read_bytes())
        kbm_files, _ = _build_game_config(conf_bak, s, kbm=True)
        size = max(len(cfg.encode(files)), len(cfg.encode(kbm_files))) + 3
        files, kbm_files = cfg.pad_to(files, size), cfg.pad_to(kbm_files, size)
        kbm_dir.mkdir(parents=True, exist_ok=True)
        for old in ("Xbox360TOC.txt",):  # written by an earlier test build; not read
            (kbm_dir / old).unlink(missing_ok=True)
        (kbm_dir / cfg.CONFIG_REL.name).write_bytes(cfg.encode(kbm_files))
    elif kbm_dir.exists():
        shutil.rmtree(kbm_dir, ignore_errors=True)

    if files is None:
        # Everything at default: put the original files back.
        shutil.copy2(conf_bak, conf)
        shutil.copy2(toc_bak, toc)
    else:
        shutil.copy2(toc_bak, toc)  # update the TOC entry starting from the original
        cfg.write(game, files)
    # Reticle size: a copy of the HUD's package (tbc_reticle.py), and its own TOC entry.
    try:
        size = int(s.get("reticle_size", 100)) if s.get("custom_reticle") else 100
        tbc_reticle.apply_reticle_size(game, toc, size)
    except PermissionError:
        raise
    except (OSError, ValueError) as e:
        summary += " (" + tr("reticle size not changed: {error}", error=e) + ")"
    return summary


def _build_game_config(conf_bak: Path, s, kbm: bool):
    """The config's files with the settings applied, for keyboard and mouse (kbm) or not, and
    the number of changes; (None, 0) when nothing differs from the original."""
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
    linear_aim = kbm  # the mouse gets the full turn speed while aiming
    look_edit = turn != 100 or linear_aim
    # With keyboard and mouse, fights are always on the mouse buttons (the triggers); with a
    # controller, on its triggers when "Fight attacks on the triggers" is ticked.
    fight_mouse = kbm or bool(s.get("trigger_attacks"))
    mouse_camera = kbm and bool(s.get("mouse_look", True))
    wide_pitch = bool(s.get("wide_pitch", True))
    no_recenter = bool(s.get("no_auto_recenter"))
    linear_stick = bool(s.get("linear_stick"))
    camera_free = wide_pitch or no_recenter
    stick_smoothing = int(s.get("stick_smoothing", 100))
    stick_camera = not mouse_camera and (turn != 100 or stick_smoothing != 100 or linear_stick)
    linear_aim = linear_aim or linear_stick
    look_edit = turn != 100 or linear_aim

    if kbm and not s.get("kbm_aim_assist", False):
        # The game's aim assist, the reticle sticking to a target while either of you strafes
        # (Hm_Engine.HmTargetCirclePositionAdjuster; its own comment on MaxStrafeSticky: "Between
        # zero and one, represents the most assist-y number you could want"), off with keyboard and
        # mouse: made for a stick, it drags the camera against the mouse. One value for the whole
        # session, so a controller used alongside goes without it too.
        edits.append(("Xe-BourneGame.ini", "Hm_Engine.HmTargetCirclePositionAdjuster", "MaxStrafeSticky", "0"))

    if turn != 100:
        # The camera's top yaw rate (100 in the original): a cap on how fast it can turn,
        # whatever the mode's turn rate, so it scales with the turn speed too.
        edits.append(("Xe-BourneInput.ini", "BourneNative.BnPlayerInput", "MaxYawRate", f"{100 * turn / 100:g}"))

    if (not edits and fov == 100 and not look_edit and not fight_mouse and not mouse_camera and not camera_free
            and not stick_camera):
        return None, 0

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
    if mouse_camera:
        for short in ("Xe-BourneCamera.ini", "BourneCamera.ini"):
            try:
                i = cfg.find_file(files, short)
            except SystemExit:
                continue
            name, body = files[i]
            files[i] = (name, _edit_camera_for_mouse(body, int(s.get("mouse_smoothing", 10)) / 100))
    if camera_free:
        for short in ("Xe-BourneCamera.ini", "BourneCamera.ini"):
            try:
                i = cfg.find_file(files, short)
            except SystemExit:
                continue
            name, body = files[i]
            files[i] = (name, _edit_camera_freedom(body, wide_pitch, no_recenter))
    if stick_camera:
        for short in ("Xe-BourneCamera.ini", "BourneCamera.ini"):
            try:
                i = cfg.find_file(files, short)
            except SystemExit:
                continue
            name, body = files[i]
            files[i] = (name, _edit_camera_for_stick(body, turn / 100, stick_smoothing / 100, linear_stick))
    return files, (len(edits) + (fov != 100) + look_edit + fight_mouse + mouse_camera + camera_free
                   + stick_camera)


def apply_intro_skip(game: Path, skip: bool) -> None:
    """Rename the intro logo movies away (skip) or back (don't skip)."""
    movies = game / "BourneGame" / "Movies"
    for name in INTRO_MOVIES:
        on, off = movies / name, movies / (name + SKIPPED_SUFFIX)
        if skip and on.exists() and not off.exists():
            on.rename(off)
        elif not skip and off.exists() and not on.exists():
            off.rename(on)


def documents_dir():
    """The Documents folder the game puts its save data in (FOLDERID_Documents), or None."""
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes

    class GUID(ctypes.Structure):
        _fields_ = [("d1", wintypes.DWORD), ("d2", wintypes.WORD), ("d3", wintypes.WORD), ("d4", ctypes.c_ubyte * 8)]

    docs = GUID(0xFDD39AD0, 0x238F, 0x46AF, (ctypes.c_ubyte * 8)(0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7))
    out = ctypes.c_wchar_p()
    try:
        if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(docs), 0, None, ctypes.byref(out)) != 0:
            return None
        return Path(out.value)
    except (OSError, AttributeError):
        return None
    finally:
        if out:
            ctypes.windll.ole32.CoTaskMemFree(out)


def ansi_safe(path) -> bool:
    """Whether a path survives the Windows ANSI code page (older runtime builds crash on ones that don't)."""
    try:
        str(path).encode("mbcs", "strict")
        return True
    except UnicodeEncodeError:
        return False
    except LookupError:  # no "mbcs" codec: not Windows
        return True


def user_data_args(info, exe: Path) -> list:
    """Point the game's save data at <install>\\userdata when the Documents path has characters the
    ANSI code page can't hold (an Arabic or Cyrillic OneDrive Documents folder, say). Builds made
    before the SDK's UTF-8 path fix close a few seconds after starting with such a path. A
    user_data_root already set in tbc.toml is left alone."""
    docs = documents_dir()
    if docs is None or ansi_safe(docs):
        return []
    try:
        if "user_data_root" in (exe.parent / "tbc.toml").read_text(encoding="utf-8", errors="replace"):
            return []
    except OSError:
        pass
    target = logs_dir(info) / "userdata"
    if not ansi_safe(target):
        return []
    try:
        target.mkdir(parents=True, exist_ok=True)
    except OSError:
        return []
    return ["--user_data_root", str(target)]


def logs_dir(info) -> Path:
    return Path(info.get("root") or Path(info["exe"]).parent)


def user_data_dir(info) -> Path:
    """Where the game keeps its saves, achievements and caches: <install>\\userdata when user_data_args
    moves it there, a user_data_root set in tbc.toml, else the runtime's default, Documents\\tbc."""
    exe = Path(info["exe"])
    args = user_data_args(info, exe)
    if args:
        return Path(args[1])
    try:
        m = re.search(r'^\s*user_data_root\s*=\s*["\']([^"\']+)["\']', (exe.parent / "tbc.toml").read_text(
            encoding="utf-8", errors="replace"), re.M)
        if m:
            return Path(m.group(1))
    except OSError:
        pass
    if not IS_WINDOWS:
        return data_home() / "tbc"  # the runtime's default on Linux: $XDG_DATA_HOME/tbc
    docs = documents_dir() or Path.home() / "Documents"
    return docs / "tbc"


def backup_saves(info) -> Path:
    """Zips the game's user data (saves, achievements, settings), leaving out the shader caches and older
    backups, to <user data>\\backups\\saves-<date>-<time>.zip."""
    root = user_data_dir(info)
    if not root.is_dir():
        raise OSError(f"{root} doesn't exist yet: start the game once first")
    out = root / "backups" / time.strftime("saves-%Y%m%d-%H%M%S.zip")
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path in root.rglob("*"):
            rel = path.relative_to(root)
            if rel.parts and rel.parts[0] in ("cache", "backups"):
                continue
            if path.is_file():
                z.write(path, rel)
    return out


def save_display_name(header: Path):
    """A save's name as the game gave it: XCONTENT_DATA's display name (UTF-16 big-endian, 128 characters at
    offset 8) in the runtime's <name>.header, or None."""
    try:
        data = header.read_bytes()[8:8 + 256]
        name = data.decode("utf-16-be", errors="ignore").split("\0", 1)[0].strip()
        return name or None
    except OSError:
        return None


def list_saves(root: Path):
    """The game's saved games under the runtime's content folder: <user data>\\<profile xuid>\\<title id>\\
    00000001 (saved games)\\<file name>\\, each with its display name from Headers\\00000001\\<file name>.header,
    the newest file's time and the total size; newest first."""
    saves = []
    if not root.is_dir():
        return saves
    for folder in root.glob("*/*/00000001/*"):
        if not folder.is_dir():
            continue
        files = [f for f in folder.rglob("*") if f.is_file()]
        modified = max([f.stat().st_mtime for f in files] or [folder.stat().st_mtime])
        header = folder.parent.parent / "Headers" / "00000001" / (folder.name + ".header")
        saves.append({"path": folder, "name": save_display_name(header) or folder.name, "modified": modified,
                      "size": sum(f.stat().st_size for f in files)})
    return sorted(saves, key=lambda s: s["modified"], reverse=True)


def load_achievements(info, language=1):
    """The game's achievements (achievement_list.cpp writes the list on every start) with their unlock times
    (the runtime's <title id>.toml: [unlocked.<id>] filetime = <FILETIME>), or None before the first start.
    Text is in the launcher's language (an XLanguage id) when the list has it, else English, else whatever
    language the game last ran in (lists from before 0.2.9 only have that)."""
    folder = user_data_dir(info) / "achievements"
    lists = sorted(folder.glob("*-list.json")) if folder.is_dir() else []
    if not lists:
        return None
    data = json.loads(lists[0].read_text(encoding="utf-8"))
    unlocked = {}
    try:
        text = (folder / f"{data.get('title_id', '')}.toml").read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"\[unlocked\.(\d+)\]\s*filetime\s*=\s*(\d+)", text):
            unlocked[int(m.group(1))] = int(m.group(2))
    except OSError:
        pass
    for a in data.get("achievements", []):
        ft = unlocked.get(int(a["id"]))
        a["unlocked"] = None if ft is None else time.localtime(max(0, (ft - 116444736000000000) // 10_000_000))
        a["icon_path"] = folder / a["icon"] if a.get("icon") else None
        for key, plural in (("name", "names"), ("description", "descriptions"),
                            ("locked_description", "locked_descriptions")):
            texts = a.get(plural) or {}
            a[key] = texts.get(str(language)) or texts.get("1") or a.get(key, "")
    return data["achievements"]


def save_crash_report(info) -> Path:
    """Zip the game log, the newest crash files and the launcher settings onto the desktop."""
    root = logs_dir(info)
    exe_logs = Path(info["exe"]).parent / "logs"
    files = []
    for p in (root / "game.log", root / "install.log", SETTINGS_PATH, install_info_path()):
        if p.is_file():
            files.append(p)
    crash = sorted(exe_logs.glob("crash-*.*"), key=lambda p: p.stat().st_mtime, reverse=True) if exe_logs.is_dir() else []
    if crash:
        stem = crash[0].stem  # crash-<pid>: the newest crash's .txt and .dmp
        files += [p for p in crash if p.stem == stem]
    desktop = desktop_dir()
    out = desktop / f"BourneConspiracy-report-{time.strftime('%Y%m%d-%H%M%S')}.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for p in files:
            z.write(p, p.name)
    return out


def window_size(s, screen):
    """Window size for windowed mode: the chosen output size, shrunk (keeping its shape) to
    fit within 85% of the screen so a 1440p/4K window doesn't cover the whole display."""
    w, h = RES_SIZE.get(s["resolution"], (1280, 720))
    if screen:
        sw, sh = int(screen[0] * 0.85), int(screen[1] * 0.85)
        k = min(1.0, sw / w, sh / h)
        if k < 1.0:
            w, h = int(w * k) // 2 * 2, int(h * k) // 2 * 2
    return w, h


def game_args(s, screen=None):
    """ReXGlue command-line settings, only when not at default. Valued settings use the
    "--name value" form already used for --gpu_plugin; on/off settings use "--name=value"."""
    args = ["--gpu_plugin", RENDERER_PLUGIN.get(s.get("renderer"), "native")]
    if is_ultrawide(s["resolution"]):
        w, h = RES_SIZE[s["resolution"]]
        args += ["--aspect_ratio", f"{w / h:.4f}"]
    # Always pass fullscreen explicitly: the runtime's own default is fullscreen, so
    # leaving the flag out when the box is unticked still opened full screen.
    args.append("--fullscreen=true" if s["fullscreen"] else "--fullscreen=false")
    if is_ultrawide(s["resolution"]):
        w, h = RES_SIZE[s["resolution"]]
        args += ["--video_mode_width", str(w), "--video_mode_height", str(h)]
    elif s["resolution"] in RES_FLAG:
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
    scale = math.ceil(scale_factor(s["render_scale"]) - 1e-6)  # Xenos: whole numbers only
    args += ["--resolution_scale", str(scale)]
    path = RENDER_PATH_FLAG.get(s.get("render_path"), "rov" if scale > 1 else None)
    if path and IS_WINDOWS:
        args += ["--render_target_path_d3d12", path]
    args += ["--swap_post_effect", AA_FLAG.get(s["aa"], "none")]
    if s.get("upscale") in UPSCALE_FLAG and IS_WINDOWS:
        args += ["--present_effect", UPSCALE_FLAG[s["upscale"]]]
    # Guest vsync (the plugin's imitation of the Xbox 360 vblank): off, with tbc.exe's own
    # frame limiter holding the frame rate, unless "original frame pacing" is ticked.
    args.append("--vsync=true" if s.get("original_pacing") else "--vsync=false")
    # Display VSync: unticked lets presents tear instead of waiting for the display, which
    # also stops a present from stalling the graphics queue the emulation uses.
    if IS_WINDOWS:
        args.append("--d3d12_allow_variable_refresh_rate_and_tearing=" + ("false" if s["vsync"] else "true"))
    rate, _ = FPS_MODE[s["fps"]]
    args.append(f"--frame_rate={rate}")
    counter = s.get("fps_counter", "Off")
    if counter != "Off":
        args.append("--show_fps=true")
        if counter.startswith("Detailed"):
            args.append("--show_fps_detail=true")
    if not s.get("achievement_popups", True):
        args.append("--achievement_popups=false")
    # Controller vibration: src/vibration.cpp scales the game's rumble; at 0 the runtime's own switch turns it
    # off as well (rexglue InputSystem zeroes every rumble at XamInputSetState).
    strength = int(s.get("vibration_strength", 100))
    if strength < 100:
        args.append(f"--vibration_strength={strength}")
    if strength <= 0:
        args.append("--vibration=false")
    args.append(f"--user_language={LANGUAGES.get(s.get('language'), 1)}")
    if int(s.get("brightness", 100)) != 100:
        args.append(f"--brightness={int(s['brightness']) / 100:.2f}")  # the native renderer's last pass
    if s.get("trigger_attacks") and not s.get("kbm"):
        # Fight prompts show light / heavy attack on RT / LT (R2 / L2) instead of X / Y
        # (keyboard_prompts.cpp), to match the trigger bindings written to Xe-BourneInput.ini.
        args.append("--pad_trigger_attacks=true")
    if s.get("kbm"):
        # Keyboard and mouse only: no controller driver. The config this mode writes (fight
        # attacks off X / Y and onto the triggers, 6x camera turn rates for mouse look) would
        # make a controller misbehave, and the prompts show keys.
        args.append("--input_backend=none")
        args += ["--mnk_mode=true", "--mnk_mouse=true",
                 "--mnk_sensitivity", f"{int(s.get('mouse_sensitivity', 100)) / 100:.2f}",
                 # Mouse movement starts just past the game's stick dead zone (the same value
                 # written to Xe-BourneInput.ini above), so slow aiming isn't swallowed by it.
                 # Needs SDK patch 0004; a stock SDK ignores it.
                 "--mnk_mouse_deadzone", f"{int(s['deadzone']) / 100:.2f}"]
        # Mouse look: the sensitivity slider's 100% is 2.0 (0.022 degrees a count x 2).
        args.append(f"--mouse_look_sensitivity={2 * int(s.get('mouse_sensitivity', 100)) / 100:.2f}")
        # Aiming (over the shoulder, shooting from cover): a percentage of the mouse sensitivity.
        args.append(f"--mouse_look_aim_sensitivity={int(s.get('aim_sensitivity', 100)) / 100:.2f}")
        # The full-stick turn rate the camera edit gives the game (65 degrees a second x the factor).
        args.append(f"--mouse_look_full_rate={65 * MOUSE_RATE_FACTOR}")
        # Direct (the default): the mouse's movement goes straight into the camera's angles;
        # unticked, it's turned into a right-stick push as in 0.2.5-0.2.6.
        if not s.get("mouse_look_direct", True):
            args.append("--mouse_look_direct=false")
        if not s.get("mouse_look", True):
            args.append("--mouse_look=false")
        # Fight prompts show light / heavy attack as [LMB] / [RMB] (keyboard_prompts.cpp), to
        # match the trigger bindings written to Xe-BourneInput.ini above.
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
        # A hair over the exact scale, so 1600 / 720 gives 1600 lines rather than 1599.
        k = scale_factor(s["render_scale"])
        env["BOTS_GFX_RENDER_SCALE"] = f"{k:g}" if k in (1, 1.5, 2, 3) else f"{k + 1e-6:.6f}"
        # FXAA in the native renderer's last pass (native-plugin/patches/0010); Xenos takes --swap_post_effect.
        fxaa = {"FXAA": "1", "FXAA (strong)": "2"}.get(s.get("aa"))
        if fxaa:
            env["BOTS_GFX_FXAA"] = fxaa
        aniso = ANISOTROPY.get(s.get("texture_filtering"), 0)
        if aniso:
            env["BOTS_GFX_FORCE_ANISO"] = str(aniso)
        # Second render thread (native-plugin/patches/0022-record-thread, from the 007 port): the
        # renderer hands its Vulkan recording to a worker thread. Always set, 1 or 0, so a value
        # left in the environment can't override the launcher.
        env["BOTS_GFX_RECORD_THREAD"] = "1" if s.get("render_thread", True) else "0"
    return env


# --------------------------------------------------------------------------
# UI
# --------------------------------------------------------------------------

def link_button(parent, text, cmd):
    """A small red button with white text: the launcher's secondary actions (Full page with notes, ...)."""
    return tk.Button(parent, text=text, command=cmd, font=("Segoe UI", 9), fg=ON_ACCENT, bg=ACCENT,
                     activebackground=ACCENT_HOVER, activeforeground=ON_ACCENT, bd=0, padx=10, pady=3, cursor="hand2")


# The settings layout follows the 007 launcher's (thegh0sts/007-QOS-X360-PORT, 27 Nov), in this launcher's colours:
# each setting's name with its note under it on the left, its control in the right-hand third (button groups,
# drop-downs, sliders and button stacks, all as wide as the column, so they line up).

def two_columns(box):
    """A settings page's grid: names and notes in the left two thirds, the controls in the right third."""
    box.columnconfigure(0, weight=2, uniform="settings")
    box.columnconfigure(1, weight=1, uniform="settings")


def wrapping(label):
    """Wrap the label's text at whatever width its grid cell gives it (the left column), so long lines go on to the
    next line instead of widening the column."""
    label.configure(anchor="w", justify="left", wraplength=470)
    label.bind("<Configure>", lambda e: label.configure(wraplength=max(120, e.width - 2)))
    return label


def note(box, row, text):
    """A small grey explanation of a setting. Right after a setting (row - 1 is its row) it goes in the setting's own
    cell under its name, so the name and the note share one grid row and the control sits centred beside both;
    anywhere else it gets a row of its own in the left column."""
    cell = getattr(box, "setting_cells", {}).get(row - 1)
    label = wrapping(tk.Label(cell or box, text=tr(text), font=("Segoe UI", 8), fg=DIM, bg=PANEL))
    if cell:
        label.pack(fill="x", pady=(1, 0))
    else:
        label.grid(row=row, column=0, sticky="ew", pady=(0, 4), padx=(0, 16))
    return label


def setting_name(box, row, text):
    """A setting's name in the left column, wrapped like the notes: in a cell of its own that its note joins.
    Returns the cell (hidden with the control when the setting doesn't apply)."""
    cell = tk.Frame(box, bg=PANEL)
    cell.grid(row=row, column=0, sticky="ew", pady=(5, 5), padx=(0, 16))
    if not hasattr(box, "setting_cells"):
        box.setting_cells = {}
    box.setting_cells[row] = cell
    wrapping(tk.Label(cell, text=tr(text), font=("Segoe UI", 10), fg=FG, bg=PANEL)).pack(fill="x")
    return cell


def dropdown(parent, bg, labels, current, choose, width=None):
    """A drop-down in the toggle-button-group style: a dark field with a thin grey border, the chosen label and an
    arrow; a click opens the list. current() gives the chosen index, choose(i) picks one. Up/Down change it and
    Space/Return open the list when it has focus. width: in characters; without it the field takes the width its
    layout gives it. The returned frame's refresh() shows a change made elsewhere."""
    holder = tk.Frame(parent, bg=BORDER, padx=1, pady=1, highlightthickness=1, highlightbackground=bg,
                      highlightcolor=ACCENT_HOVER, takefocus=1)
    face = tk.Label(holder, font=("Segoe UI", 9), bg=FIELD_BG, fg=FG, anchor="w", padx=8, pady=2, cursor="hand2",
                    width=width or 1)
    arrow = tk.Label(holder, text="▾", font=("Segoe UI", 9), bg=FIELD_BG, fg=DIM, padx=8, pady=2, cursor="hand2")
    face.pack(side="left", fill="x", expand=True)
    arrow.pack(side="right")

    def refresh():
        i = current()
        face.config(text=labels[i] if 0 <= i < len(labels) else "")

    def pick(i):
        if 0 <= i < len(labels) and i != current():
            choose(i)
        if holder.winfo_exists():
            refresh()

    def open_list(_=None):
        """The list: a borderless window as wide as the field, just below it (above it when it wouldn't fit on the
        screen), the current choice lit. A click picks; a click outside or Escape closes it; Up/Down/Return work."""
        holder.focus_set()
        popup = tk.Toplevel(holder)
        popup.overrideredirect(True)
        popup.attributes("-topmost", True)
        frame = tk.Frame(popup, bg=BORDER, padx=1, pady=1)
        frame.pack(fill="both", expand=True)
        rows, lit_row = [], [current()]

        def paint():
            for j, row in enumerate(rows):
                on = j == lit_row[0]
                row.config(bg=ACCENT if on else FIELD_BG, fg=ON_ACCENT if on else FG)

        def close(_=None):
            if popup.winfo_exists():
                popup.grab_release()
                popup.destroy()
            if holder.winfo_exists():
                holder.focus_set()
            return "break"

        def choose_row(j):
            close()
            pick(j)
            return "break"

        for j, label in enumerate(labels):
            row = tk.Label(frame, text=label, font=("Segoe UI", 9), anchor="w", padx=8, pady=3, cursor="hand2")
            row.pack(fill="x")
            row.bind("<Enter>", lambda _, j=j: (lit_row.__setitem__(0, j), paint()))
            row.bind("<ButtonRelease-1>", lambda _, j=j: choose_row(j))
            rows.append(row)
        paint()

        def outside(event):
            inside = popup.winfo_containing(event.x_root, event.y_root)
            if inside is None or inside.winfo_toplevel() is not popup:
                close()

        def move(step):
            lit_row[0] = max(0, min(len(rows) - 1, lit_row[0] + step))
            paint()
            return "break"

        popup.bind("<Button-1>", outside)
        popup.bind("<Escape>", close)
        popup.bind("<Up>", lambda _: move(-1))
        popup.bind("<Down>", lambda _: move(1))
        popup.bind("<Return>", lambda _: choose_row(lit_row[0]))
        popup.update_idletasks()
        w = max(holder.winfo_width(), popup.winfo_reqwidth())
        h = popup.winfo_reqheight()
        x, y = holder.winfo_rootx(), holder.winfo_rooty() + holder.winfo_height()
        if y + h > holder.winfo_screenheight():
            y = holder.winfo_rooty() - h
        popup.geometry(f"{w}x{h}+{x}+{y}")
        popup.grab_set()
        popup.focus_set()
        return "break"

    for w in (face, arrow):
        w.bind("<Button-1>", open_list)
        w.bind("<Enter>", lambda _: (face.config(bg=FIELD_HOVER), arrow.config(bg=FIELD_HOVER)))
        w.bind("<Leave>", lambda _: (face.config(bg=FIELD_BG), arrow.config(bg=FIELD_BG)))
    holder.bind("<space>", open_list)
    holder.bind("<Return>", open_list)
    holder.bind("<Up>", lambda _: (pick(current() - 1), "break")[1])
    holder.bind("<Down>", lambda _: (pick(current() + 1), "break")[1])
    holder.refresh = refresh

    def set_labels(new):
        labels[:] = list(new)
        refresh()
    holder.set_labels = set_labels
    refresh()
    return holder


def button_group(parent, bg, labels, current, choose, var, per_row=5):
    """A toggle button group: joined buttons, one per label, the current one lit, sharing the control column
    equally; more than per_row wrap to another row. current() gives the lit button's index, choose(i) picks one;
    redrawn whenever var changes. Click a button, or Left/Right when the group has focus (Space moves to the next
    one). The returned frame's set_labels(labels) replaces the buttons (lists that change with other settings)."""
    holder = tk.Frame(parent, bg=BORDER, padx=1, pady=1, highlightthickness=1, highlightbackground=bg,
                      highlightcolor=ACCENT_HOVER, takefocus=1)
    buttons = []

    def draw(*_):
        lit = current()
        for i, b in enumerate(buttons):
            b.config(bg=ACCENT if i == lit else FIELD_BG, fg=ON_ACCENT if i == lit else DIM)

    def pick(i):
        if buttons:
            choose(max(0, min(len(buttons) - 1, i)))
        holder.focus_set()
        return "break"

    def hover(i, entering):
        if i != current():
            buttons[i].config(bg=FIELD_HOVER if entering else FIELD_BG)

    def set_labels(new):
        for b in buttons:
            b.destroy()
        buttons.clear()
        new = list(new)
        cols = min(per_row, len(new)) or 1
        for c in range(per_row):
            holder.columnconfigure(c, weight=1 if c < cols else 0, uniform="buttons" if c < cols else "")
        for i, label in enumerate(new):
            row, col = divmod(i, cols)
            # the last button of a short last row takes the rest of the row
            span = cols - col if i == len(new) - 1 else 1
            b = tk.Label(holder, text=label, font=("Segoe UI", 9), width=1, pady=2, cursor="hand2")
            b.grid(row=row, column=col, columnspan=span, sticky="nsew", padx=(1 if col else 0, 0),
                   pady=(1 if row else 0, 0))
            b.bind("<Button-1>", lambda _, i=i: pick(i))
            b.bind("<Enter>", lambda _, i=i: hover(i, True))
            b.bind("<Leave>", lambda _, i=i: hover(i, False))
            buttons.append(b)
        draw()

    holder.bind("<Left>", lambda _: pick(current() - 1))
    holder.bind("<Right>", lambda _: pick(current() + 1))
    holder.bind("<space>", lambda _: pick((current() + 1) % max(1, len(buttons))))
    holder.set_labels = set_labels
    holder.refresh = draw
    var.trace_add("write", draw)
    set_labels(labels)
    return holder


def slide_switch(parent, v, bg):
    """An on/off slide switch for the BooleanVar v, on background bg: "Close launcher when the game starts" in the
    bottom bar, where a button group has no room. Click it, or Space when it has focus."""
    switch = tk.Canvas(parent, width=44, height=22, bg=bg, highlightthickness=1, highlightbackground=bg,
                       highlightcolor=ACCENT_HOVER, cursor="hand2", takefocus=1)

    def draw(*_):
        on = bool(v.get())
        track = ACCENT if on else "#3a404c"
        switch.delete("all")
        switch.create_oval(1, 1, 21, 21, fill=track, outline=track)
        switch.create_oval(23, 1, 43, 21, fill=track, outline=track)
        switch.create_rectangle(11, 1, 33, 21, fill=track, outline=track)
        x = 25 if on else 4
        switch.create_oval(x, 4, x + 15, 19, fill=FG, outline="")

    def flip(_=None):
        v.set(not v.get())
        return "break"

    switch.bind("<Button-1>", flip)
    switch.bind("<space>", flip)
    v.trace_add("write", draw)
    draw()
    return switch


def resolution_label(value):
    """An output or render resolution on its button: its height ("1920×1080 (...)" is "1080p"); "4K" and the
    original stay as they are named; an ultrawide output is "UW" and its height."""
    if value == RESOLUTIONS[0]:
        return "720p"
    m = re.match(r"^(\d+)×(\d+)", value)
    if not m:
        return value
    if value in ULTRAWIDE:
        return f"UW {m.group(2)}p"
    return f"{m.group(2)}p"


# Button texts for the settings shown as button groups: shorter than the saved values (which stay as they were,
# so existing settings keep working). Translated with tr_label.
CHOICE_LABELS = {
    "renderer": {RENDERERS[0]: "Native", RENDERERS[1]: "Xenos"},
    "render_path": {RENDER_PATHS[0]: "Auto", RENDER_PATHS[1]: "RTV", RENDER_PATHS[2]: "ROV"},
    "upscale": {UPSCALE[0]: "Bilinear", UPSCALE[1]: "FSR", UPSCALE[2]: "CAS"},
    "aa": {AA[0]: "Off", AA[1]: "FXAA", AA[2]: "FXAA strong"},
    "fps": {FPS[0]: "30", FPS[1]: "60", FPS[2]: "120", FPS[3]: "144", FPS[4]: "∞"},
    "fps_counter": {FPS_COUNTER[0]: "Off", FPS_COUNTER[1]: "FPS", FPS_COUNTER[2]: "Detailed"},
    "input_device": {INPUT_DEVICES[0]: "Controller", INPUT_DEVICES[1]: "KBM"},
    "texture_filtering": {TEXTURE_FILTERING[0]: "Original", TEXTURE_FILTERING[1]: "4x", TEXTURE_FILTERING[2]: "8x",
                          TEXTURE_FILTERING[3]: "16x"},
}

# The Credits tab: (name, what it gave the port, website or None), as in the README's Credits. Names aren't
# translated; the texts are.
CREDITS = [
    ("ReXGlue SDK", "The recompiler and runtime that turn the Xbox 360 game into a PC program, built from the Xenia "
                    "emulator.", "https://github.com/rexglue/rexglue-sdk"),
    ("Skullburglar", "The native Vulkan renderer, the crash reports and the leak scanner. BSD 3-Clause licence.",
     "https://github.com/nlwstein/skullburglar"),
    ("Xenia", "The Xbox 360 emulator the SDK's runtime comes from.", "https://xenia.jp"),
    ("Table Tennis PC", "The frame pacing approach. MIT licence.",
     "https://github.com/davidptrssvea/Table-Tennis-Rockstar-PC"),
    ("WetRecomp", "By nikolaygorb: the GPU-wait hook, display refresh matching and the XMA audio decoder fixes. MIT "
                  "licence.", "https://github.com/nikolaygorb/WetRecomp"),
    ("AMD FidelityFX", "FSR 1 upscaling and CAS sharpening.", "https://gpuopen.com"),
]


class SettingsView(tk.Frame):
    def __init__(self, master, info, on_reinstall, on_locate, on_language=None, settings=None, saved=None):
        """settings / saved: when rebuilt for another launcher language (on_language), the settings as they
        stood (unsaved changes kept) and what was saved, so leaving still asks about the same changes."""
        super().__init__(master, bg=BG)
        self.info = info
        self.s = dict(settings) if settings else load_settings()
        self.on_language = on_language
        self.vars = {}
        self.combos = {}  # each list setting's control (button group or drop-down)
        self.combo_values = {}  # the settings' own (English) values behind each control's labels
        self.combo_labels = {}  # each list setting's name cell (hidden with the control)
        self.combo_texts = {}  # each list setting's value -> label function
        # The window has a fixed size, so each tab scrolls when its settings don't fit. The wheel scrolls the tab
        # that's showing, wherever the pointer is in it.
        self.scrollers = {}
        self.bind_all("<MouseWheel>", self._wheel)

        head = tk.Frame(self, bg=BG)
        head.pack(fill="x", padx=28, pady=(20, 4))
        tk.Label(head, text=APP_NAME.upper(), font=("Segoe UI", 22, "bold"), fg=ACCENT, bg=BG).pack(side="left")
        tk.Label(head, text="PC PORT", font=("Segoe UI", 9), fg=DIM, bg=BG
                 ).pack(side="left", anchor="s", padx=(12, 0), pady=(0, 6))
        # The launcher's language, top right: "Same as game" or one of the five, each in its own language.
        choices = ["same"] + list(launcher_text.LANGUAGE_CODES)
        names = [tr("Same as game")] + [launcher_text.NATIVE_NAMES[c] for c in launcher_text.LANGUAGE_CODES]
        current = self.s.get("launcher_language", "same")
        self.vars["launcher_language"] = tk.StringVar(value=current if current in choices else "same")
        dropdown(head, BG, names, lambda: choices.index(self.vars["launcher_language"].get()),
                 lambda i: self._launcher_language(choices[i]), width=16).pack(side="right", anchor="s")
        tk.Label(head, text=tr("Launcher language"), font=("Segoe UI", 9), fg=DIM, bg=BG
                 ).pack(side="right", anchor="s", padx=(16, 6), pady=(0, 2))
        tk.Label(head, text=tr("Launcher {version}", version=VERSION), font=("Segoe UI", 9), fg=DIM, bg=BG
                 ).pack(side="right", anchor="s", pady=(0, 2))
        if launcher_text.language() != "en":
            tk.Label(self, text=tr("Translated by AI, not yet checked by native speakers."), font=("Segoe UI", 8),
                     fg=DIM, bg=BG).pack(anchor="e", padx=28)
        # Filled in by the update check, if a newer release is out.
        self.update_line = tk.Frame(self, bg=BG)
        self.update_line.pack(fill="x", padx=28)
        # The game was built before the last version with game changes (GAME_VERSION): they aren't in it until
        # it's reinstalled (rebuilt), so say so where it can't be missed.
        if version_key(built_version(info)) < version_key(GAME_VERSION):
            box = tk.Frame(self, bg=ACCENT)
            box.pack(fill="x", padx=28, pady=(6, 2))
            tk.Label(box, text=tr("Your game was built with {built}; this launcher is {version}. Reinstall to rebuild "
                                  "the game with this version's changes.", built=built_version(info), version=VERSION),
                     font=("Segoe UI", 10, "bold"), fg=ON_ACCENT, bg=ACCENT, wraplength=700, justify="left"
                     ).pack(side="left", padx=10, pady=8)
            tk.Button(box, text=tr("Reinstall now"), command=on_reinstall, font=("Segoe UI", 10, "bold"),
                      bd=0, padx=10, pady=4).pack(side="right", padx=10)
            self.needs_rebuild = True
        else:
            self.needs_rebuild = False
        self._update_shown = False
        self.check_updates(manual=False)
        self.check_achievements()

        # Settings in tabs: a list down the left, level with the page, the chosen tab's settings on the right.
        body = tk.Frame(self, bg=BG)
        nav = tk.Frame(body, bg=BG, width=180)
        nav.pack(side="left", fill="y")
        nav.pack_propagate(False)
        self.pages_frame = tk.Frame(body, bg=PANEL)
        self.pages_frame.pack(side="left", fill="both", expand=True)
        self.pages, self.tab_buttons = {}, {}
        for name in TABS:
            b = tk.Button(nav, text=tr(name), anchor="w", font=("Segoe UI", 11, "bold"), bd=0, padx=16, pady=9,
                          cursor="hand2", command=lambda n=name: self.show_tab(n))
            b.pack(fill="x", pady=(0, 2))
            self.tab_buttons[name] = b

        # Game files: what each thing is on the left, its buttons on the right.
        box = self._page("Game files")
        setting_name(box, 0, "Install location")
        note(box, 1, str(info['game']))
        self._button_stack(box, 0, [(tr("Reinstall from a disc image..."), on_reinstall),
                                    (tr("Use a different install..."), on_locate)], pady=(2, 18))
        setting_name(box, 2, "Help")
        note(box, 3, "If the game crashes or misbehaves: the logs show what happened, and a crash report gathers them "
                     "into one file to send with a bug report.")
        self._button_stack(box, 2, [(tr("Open logs"), self.open_logs),
                                    (tr("Save crash report"), self.crash_report),
                                    (tr("Check for updates"), lambda: self.check_updates(manual=True))])
        self._subheading(box, 4, "Game")
        self._combo(box, 5, "language", "Game language", list(LANGUAGES))
        note(box, 6, "The game's voices, subtitles and text, all in this language (each language has only its own "
                     "voices and subtitles). The achievement names follow it too.")

        box = self._page("Display")
        self._choice(box, 0, "renderer", "Renderer", RENDERER_CHOICES)
        note(box, 1, "Native (Vulkan) is recommended. Xenos (Direct3D 12) is the fallback if the native renderer has "
                     "trouble on your PC.")
        self._toggle(box, 2, "fullscreen", "Fullscreen")
        self._choice(box, 3, "resolution", "Output resolution", self._resolutions(), per_row=4,
                     texts=resolution_label)
        note(box, 4, "The size of the picture on your screen. Ultrawide outputs (UW) are experimental and need the "
                     "native renderer.")
        self._choice(box, 5, "render_scale", "Render resolution (sharper, slower)", render_scales(self.s["resolution"]),
                     texts=resolution_label)
        note(box, 6, "The game draws at 720p; this is the resolution the renderer draws it at. Higher is sharper and "
                     "slower.")
        self.vars["renderer"].trace_add("write", lambda *_: self._display_changed())
        self.vars["resolution"].trace_add("write", lambda *_: self._display_changed())
        # The achievements' names follow the game's language: rebuilt when it changes. With the launcher's
        # language "Same as game", the whole launcher follows it too.
        self.vars["language"].trace_add("write", lambda *_: self._game_language_changed())
        self._choice(box, 7, "render_path", "Graphics path", RENDER_PATHS)
        note(box, 8, "Xenos only: how it imitates the Xbox 360's framebuffer memory. Auto uses ROV above 720p.")
        self._choice(box, 9, "upscale", "Upscaling filter", UPSCALE if IS_WINDOWS else UPSCALE[:1])
        note(box, 10, "How the picture is scaled to your screen. FSR gives sharper 3D but softer text; CAS only "
                      "sharpens.")
        self._choice(box, 11, "aa", "Anti-aliasing", AA)
        self._display_changed()  # hide the Xenos-only settings for the native renderer
        self._choice(box, 12, "fps", "Frame rate", FPS)
        # Above 60, scripted scenes can stall (README, Known issues); the driver's frame generation
        # on top of 60 is the smooth option until that's fixed.
        note(box, 13, "30 is the original. Above 60 is experimental: some scripted scenes can stall. For smoother "
                      "motion, keep 60 and turn on your graphics driver's frame generation (NVIDIA Smooth Motion, AMD "
                      "Fluid Motion Frames).")
        self._choice(box, 14, "fps_counter", "FPS counter", FPS_COUNTER)
        self._toggle(box, 15, "vsync", "VSync")
        note(box, 16, "Off may tear, but can be faster.")
        self._toggle(box, 17, "original_pacing", "Original Xbox 360 frame pacing")
        note(box, 18, "Paces frames as the console did: slower.")
        self._toggle(box, 19, "achievement_popups", "Achievement pop-ups")

        box = self._page("Graphics")
        for row, (key, text) in enumerate((("motion_blur", "Motion blur"), ("depth_of_field", "Depth of field"),
                                           ("bloom", "Bloom"), ("lens_flares", "Lens flares"),
                                           ("distortion", "Heat haze / distortion"))):
            self._toggle(box, row, key, text)
        self._slider(box, 5, "brightness", "Brightness (%)", 50, 200, 5)
        note(box, 6, "Native renderer only (Xenos ignores it). Black and white stay as they are.")
        self._choice(box, 7, "texture_filtering", "Texture filtering", TEXTURE_FILTERING)
        note(box, 8, "Native renderer only: keeps floors, roads and walls sharp at an angle. The HUD and text keep "
                     "the game's own filtering.")
        self._toggle(box, 9, "render_thread", "Second render thread")
        note(box, 10, "Native renderer only: records the GPU's work on another CPU thread, for a higher frame rate in "
                      "busy scenes. Turn it off if the picture shows glitches.")

        box = self._page("Camera")
        self._slider(box, 0, "fov", "Field of view (%)", 80, 140, 5)
        self._toggle(box, 1, "wide_pitch", "Look further up and down")
        self._toggle(box, 2, "no_auto_recenter", "No camera auto-recentre")
        self._toggle(box, 3, "linear_stick", "Linear stick turning")
        note(box, 4, "Controller: no turn acceleration, the stick turns at its top rate straight away.")
        self._toggle(box, 5, "custom_reticle", "Custom aiming reticle size")
        note(box, 6, "Off keeps the game's own size.")
        reticle = self._slider(box, 7, "reticle_size", "Aiming reticle size (%)", 50, 250, 10)

        def show_reticle(*_):
            for widget in reticle:
                widget.grid() if self.vars["custom_reticle"].get() else widget.grid_remove()
        self.vars["custom_reticle"].trace_add("write", show_reticle)
        show_reticle()

        box = self._page("Controls")
        # One choice of input device (27 Nov): only its settings are shown. Keyboard and mouse switches the
        # controllers off (input_backend none, and a config made for the mouse), so it's either one or the other.
        self._choice(box, 0, "input_device", "Input device", INPUT_DEVICES)
        note(box, 1, "Controller works with Xbox and PlayStation controllers, with the buttons shown to match. KBM "
                     "(keyboard and mouse) turns controllers off.")
        pad = self.pad_settings = tk.Frame(box, bg=PANEL)
        two_columns(pad)
        pad.grid(row=2, column=0, columnspan=2, sticky="ew")
        self._subheading(pad, 0, "Controller")
        self._slider(pad, 1, "deadzone", "Stick dead zone (%)", 5, 40, 1)
        self._slider(pad, 2, "turn_speed", "Stick turn speed (%)", 50, 300, 10)
        note(pad, 3, "Also raises the camera's top turn rate (full stick).")
        self._slider(pad, 4, "stick_smoothing", "Stick camera smoothing (%)", 0, 100, 5)
        note(pad, 5, "How much of the game's camera easing stays: lower is more responsive.")
        self._slider(pad, 6, "vibration_strength", "Controller vibration (%, 0 = off)", 0, 100, 5)
        self._toggle(pad, 7, "trigger_attacks", "Fight attacks on the triggers")
        note(pad, 8, "RT light, LT heavy, instead of X and Y; the prompts follow.")
        kbm = self.kbm_settings = tk.Frame(box, bg=PANEL)
        two_columns(kbm)
        kbm.grid(row=3, column=0, columnspan=2, sticky="ew")
        self._subheading(kbm, 0, "Keyboard and mouse")
        setting_name(kbm, 1, "Keyboard and mouse map")
        note(kbm, 2, "The layout is fixed: the prompts and the key images in the game are drawn for it.")
        self._button_stack(kbm, 1, [(tr("Key binds"), self.show_keys)])
        self._toggle(kbm, 3, "mouse_look", "Mouse turns the camera directly (mouse look)")
        direct = self._toggle(kbm, 4, "mouse_look_direct", "Direct (1:1)")
        note(kbm, 5, "The mouse's movement goes straight into the camera's angles. Disabled: it's turned into a "
                     "right-stick push.")
        self._toggle(kbm, 6, "kbm_aim_assist", "Aim assist")
        note(kbm, 7, "The game's sticky aim, made for a controller; off by default with keyboard and mouse.")
        self._slider(kbm, 8, "mouse_sensitivity", "Mouse sensitivity (%)", 10, 400, 5)
        self._slider(kbm, 9, "aim_sensitivity", "Aiming sensitivity (% of mouse)", 20, 200, 5)
        note(kbm, 10, "Over the shoulder and when shooting from cover.")
        self._slider(kbm, 11, "mouse_smoothing", "Mouse camera smoothing (%)", 0, 100, 5)
        note(kbm, 12, "How much of the game's camera easing stays with mouse look: lower is more responsive.")

        def show_direct(*_):  # Direct only means something with mouse look on
            for widget in direct:
                widget.grid() if self.vars["mouse_look"].get() else widget.grid_remove()
        self.vars["mouse_look"].trace_add("write", show_direct)
        show_direct()
        self.vars["input_device"].trace_add("write", lambda *_: self._input_device_changed(picked=True))
        self._input_device_changed()

        box = self._page("Saves", scroll=False)
        line = tk.Frame(box, bg=PANEL)
        line.grid(row=0, column=0, columnspan=2, sticky="w")
        self._link(line, tr("Open saves folder"), self.open_saves).pack(side="left")
        self._link(line, tr("Back up saves"), self.backup_saves).pack(side="left", padx=8)
        self._link(line, tr("Refresh"), self.refresh_saves).pack(side="left")
        self.saves_label = tk.Label(box, text="", font=("Segoe UI", 9), fg=DIM, bg=PANEL, anchor="w", justify="left",
                                    wraplength=600)
        self.saves_label.grid(row=1, column=0, columnspan=2, sticky="w", pady=(10, 6))
        style = ttk.Style(self)
        style.configure("Saves.Treeview", background=BG, fieldbackground=BG, foreground=FG, rowheight=24,
                        borderwidth=0)
        style.configure("Saves.Treeview.Heading", font=("Segoe UI", 9, "bold"))
        tree_box = tk.Frame(self.pages["Saves"], bg=PANEL)
        tree_box.pack(fill="both", expand=True, pady=(4, 0))
        self.saves_tree = ttk.Treeview(tree_box, style="Saves.Treeview", show="headings",
                                       columns=("name", "modified", "size", "folder"))
        for col, text, width in (("name", "Save", 220), ("modified", "Last saved", 150), ("size", "Size", 70),
                                 ("folder", "Folder", 160)):
            self.saves_tree.heading(col, text=tr(text, "column"), anchor="w")
            self.saves_tree.column(col, width=width, anchor="w", stretch=col == "name")
        bar = ttk.Scrollbar(tree_box, orient="vertical", command=self.saves_tree.yview)
        self.saves_tree.configure(yscrollcommand=bar.set)
        self.saves_tree.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")
        self.saves_tree.bind("<Double-1>", lambda e: self.open_save_folder())

        self._page("Achievements", scroll=False)
        self.achievements_box = tk.Frame(self.pages["Achievements"], bg=PANEL)
        self.achievements_box.pack(fill="both", expand=True)

        # Credits: what the port is built on, as in the README's Credits, laid out like the settings: what it is on
        # the left, its website on the right.
        box = self._page("Credits")
        row = 0
        for title, text, url in CREDITS:
            setting_name(box, row, title)
            note(box, row + 1, text)
            if url:
                self._button_stack(box, row, [(tr("Website"), lambda u=url: webbrowser.open(u))])
            row += 2
        note(box, row, "The Bourne Conspiracy is a trademark of its owners. This project is not affiliated with or "
                       "endorsed by Microsoft, Activision, High Moon Studios or the Ludlum estate.")
        self.show_tab(self.s.get("tab") if self.s.get("tab") in TABS else "Display")

        foot = tk.Frame(self, bg=BG)
        foot.pack(side="bottom", fill="x", padx=28, pady=18)
        tk.Button(foot, text=tr("Play"), width=16, command=self.play, font=("Segoe UI", 13, "bold"), bg=ACCENT,
                  fg=ON_ACCENT, activebackground=ACCENT_HOVER, activeforeground=ON_ACCENT, bd=0, pady=8
                  ).pack(side="left")
        tk.Button(foot, text=tr("Save settings"), width=13, command=self.save, font=("Segoe UI", 12), bd=0, pady=8,
                  fg=FG, bg=FIELD_BG, activebackground=FIELD_HOVER, activeforeground=FG,
                  highlightthickness=1, highlightbackground=ACCENT).pack(side="left", padx=(10, 0))
        tk.Button(foot, text=tr("Quit"), width=10, command=self.quit_launcher, font=("Segoe UI", 12),
                  bd=0, pady=8).pack(side="left", padx=10)
        tk.Button(foot, text=tr("Defaults"), width=10, command=self.reset, font=("Segoe UI", 11),
                  bd=0, pady=6).pack(side="right")
        v = self.vars["close_on_play"] = tk.BooleanVar(value=self.s["close_on_play"])
        slide_switch(foot, v, BG).pack(side="right", padx=(6, 10))
        tk.Label(foot, text=tr("Close launcher when the game starts"), font=("Segoe UI", 9), fg=DIM, bg=BG
                 ).pack(side="right")

        self.status = tk.StringVar(value=tr("Experimental options may cause glitches. Set them back to default to "
                                            "restore the original game."))
        tk.Label(self, textvariable=self.status, font=("Segoe UI", 9), fg=DIM, bg=BG, wraplength=700,
                 justify="left").pack(side="bottom", anchor="w", padx=28)
        body.pack(fill="both", expand=True, padx=28, pady=(14, 4))
        # What's saved, to ask about unsaved changes on quitting (kept through a language switch).
        self.saved = saved if saved is not None else self._snapshot()

    # widgets ------------------------------------------------------------
    def _page(self, title, scroll=True):
        """A tab's page. With scroll, its settings sit in a canvas with a scroll bar that shows when they don't fit;
        without (Saves, Achievements), the page lays out its own scrolling list."""
        if not scroll:
            page = tk.Frame(self.pages_frame, bg=PANEL, padx=22, pady=18)
            page.place(relx=0, rely=0, relwidth=1, relheight=1)
            box = tk.Frame(page, bg=PANEL)
            box.pack(fill="x")
            box.columnconfigure(1, weight=1)
            self.pages[title] = page
            return box
        page = tk.Frame(self.pages_frame, bg=PANEL)
        page.place(relx=0, rely=0, relwidth=1, relheight=1)
        canvas = tk.Canvas(page, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(page, orient="vertical", command=canvas.yview)
        inner = tk.Frame(canvas, bg=PANEL, padx=22, pady=18)
        window = canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side="left", fill="both", expand=True)

        def fit(_=None):
            canvas.configure(scrollregion=(0, 0, inner.winfo_reqwidth(), inner.winfo_reqheight()))
            if inner.winfo_reqheight() > canvas.winfo_height() > 1:
                if not bar.winfo_ismapped():
                    bar.pack(side="right", fill="y", before=canvas)
            else:
                if bar.winfo_ismapped():
                    bar.pack_forget()
                canvas.yview_moveto(0)

        inner.bind("<Configure>", fit)
        canvas.bind("<Configure>", lambda e: (canvas.itemconfigure(window, width=e.width), fit()))
        box = tk.Frame(inner, bg=PANEL)
        box.pack(fill="x")
        two_columns(box)
        self.pages[title] = page
        self.scrollers[title] = canvas
        return box

    def _wheel(self, event):
        canvas = self.scrollers.get(self.s.get("tab"))
        try:
            if canvas is None or not canvas.winfo_exists():
                return
            first, last = canvas.yview()
            if first > 0 or last < 1:
                canvas.yview_scroll(-1 * (event.delta // 120 or (1 if event.delta > 0 else -1)), "units")
        except tk.TclError:  # a view the launcher has already swapped out
            pass

    def _subheading(self, box, row, text):
        tk.Label(box, text=tr(text), font=("Segoe UI", 9, "bold"), fg=DIM, bg=PANEL
                 ).grid(row=row, column=0, columnspan=2, sticky="w", pady=(14, 4))

    def show_tab(self, name):
        self.pages[name].tkraise()
        if name == "Saves":
            self.refresh_saves()
        elif name == "Achievements":
            self.refresh_achievements()
        for n, b in self.tab_buttons.items():
            on = n == name
            b.configure(bg=PANEL if on else BG, fg=FG if on else DIM, activebackground=PANEL,
                        activeforeground=FG)
        self.s["tab"] = name  # reopened on this tab (saved with the settings)

    def _link(self, parent, text, cmd):
        return link_button(parent, text, cmd)

    def _button(self, parent, bg, text, cmd):
        """A button in the toggle-button-group style (an unlit button of a group): dark, thin grey border, focus
        outline. Click, or Space/Return when it has focus."""
        holder = tk.Frame(parent, bg=BORDER, padx=1, pady=1, highlightthickness=1, highlightbackground=bg,
                          highlightcolor=ACCENT_HOVER, takefocus=1)
        face = tk.Label(holder, text=text, font=("Segoe UI", 9), bg=FIELD_BG, fg=FG, width=1, pady=2,
                        cursor="hand2")
        face.pack(fill="x")
        face.bind("<Enter>", lambda _: face.config(bg=FIELD_HOVER))
        face.bind("<Leave>", lambda _: face.config(bg=FIELD_BG))
        face.bind("<ButtonRelease-1>", lambda _: (cmd(), "break")[1])
        for key in ("<space>", "<Return>"):
            holder.bind(key, lambda _: (cmd(), "break")[1])
        return holder

    def _button_stack(self, box, row, buttons, pady=2):
        """The right-hand column of a row: one or more buttons, one under another, as wide as the column."""
        stack = tk.Frame(box, bg=PANEL)
        stack.grid(row=row, column=1, sticky="ew", pady=pady)
        for i, (text, cmd) in enumerate(buttons):
            self._button(stack, PANEL, text, cmd).pack(fill="x", pady=(0 if i == 0 else 4, 0))
        return stack

    def _toggle(self, box, row, key, text):
        """An on/off setting: its name on the left, an Enabled | Disabled button group on the right. Returns the
        name cell and the group (to hide them together)."""
        v = self.vars[key] = tk.BooleanVar(value=bool(self.s[key]))
        cell = setting_name(box, row, text)
        group = button_group(box, PANEL, [tr("Enabled"), tr("Disabled")], lambda: 0 if v.get() else 1,
                             lambda i: v.set(i == 0), v)
        group.grid(row=row, column=1, sticky="ew", pady=2)
        return cell, group

    def _input_device_changed(self, picked=False):
        """Show only the chosen input device's settings. KBM picked: mouse look and Direct (1:1) start enabled, as
        the defaults, whatever they were left at."""
        kbm = self.vars["input_device"].get() == INPUT_DEVICES[1]
        if kbm and picked:
            self.vars["mouse_look"].set(True)
            self.vars["mouse_look_direct"].set(True)
        if kbm:
            self.pad_settings.grid_remove()
            self.kbm_settings.grid()
        else:
            self.kbm_settings.grid_remove()
            self.pad_settings.grid()

    def _choice(self, box, row, key, text, values, per_row=5, texts=None):
        """A settings row choosing one of a few values with a toggle button group. The buttons show texts(value)
        (default: CHOICE_LABELS, else the value), translated; the variable holds the value itself, so the settings
        file doesn't change. The values can change later (_set_combo_values)."""
        values = list(values)
        v = self.vars[key] = tk.StringVar(value=self.s[key] if self.s[key] in values else DEFAULTS[key])
        self.combo_values[key] = values
        labels = CHOICE_LABELS.get(key, {})
        self.combo_texts[key] = texts or (lambda value: labels.get(value, value))
        cell = setting_name(box, row, text)
        group = button_group(box, PANEL, self._choice_labels(key),
                             lambda: self.combo_values[key].index(v.get()) if v.get() in self.combo_values[key] else -1,
                             lambda i: v.set(self.combo_values[key][i]), v, per_row)
        group.grid(row=row, column=1, sticky="ew", pady=2)
        self.combos[key] = group
        self.combo_labels[key] = cell

    def _choice_labels(self, key):
        return [tr_label(self.combo_texts[key](value)) for value in self.combo_values[key]]

    def _combo(self, box, row, key, text, values):
        """A settings row with a drop-down (lists too long for buttons), styled and sized like the button groups."""
        values = list(values)
        v = self.vars[key] = tk.StringVar(value=self.s[key] if self.s[key] in values else DEFAULTS[key])
        self.combo_values[key] = values
        self.combo_texts[key] = lambda value: value
        cell = setting_name(box, row, text)
        combo = dropdown(box, PANEL, self._choice_labels(key),
                         lambda: self.combo_values[key].index(v.get()) if v.get() in self.combo_values[key] else 0,
                         lambda i: v.set(self.combo_values[key][i]))
        combo.grid(row=row, column=1, sticky="ew", pady=2)
        self.combos[key] = combo
        self.combo_labels[key] = cell
        v.trace_add("write", lambda *_: combo.refresh())

    def _set_combo_values(self, key, values):
        values = list(values)
        if values == self.combo_values.get(key):
            return
        self.combo_values[key] = values
        self.combos[key].set_labels(self._choice_labels(key))

    def _slider(self, box, row, key, text, lo, hi, step):
        """A settings row with a slider as wide as the control column. Returns its name cell and the slider (to hide
        them together)."""
        v = self.vars[key] = tk.IntVar(value=int(self.s[key]))
        cell = setting_name(box, row, text)
        scale = tk.Scale(box, variable=v, from_=lo, to=hi, resolution=step, orient="horizontal", length=120,
                         bg=PANEL, fg=FG, troughcolor=FIELD_BG, highlightthickness=0, bd=0, activebackground=ACCENT)
        scale.grid(row=row, column=1, sticky="ew")
        return cell, scale

    def _launcher_language(self, choice):
        """A launcher language picked: saved at once on its own (it isn't one of the settings Save is about,
        and other unsaved changes stay unsaved), then the launcher is drawn again in it, with the unsaved
        changes and the open tab kept."""
        self.vars["launcher_language"].set(choice)
        try:
            stored = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            stored = dict(DEFAULTS)
        stored["launcher_language"] = choice
        try:
            save_settings(stored)
        except OSError:
            pass
        saved = json.loads(self.saved)
        saved["launcher_language"] = choice
        self.saved = json.dumps(saved, sort_keys=True, default=str)
        if self.on_language:
            self.after_idle(self.on_language, self.collect(), self.saved)

    def _game_language_changed(self):
        if self.vars.get("launcher_language") and self.vars["launcher_language"].get() == "same" and self.on_language \
                and launcher_text.resolve("same", self.vars["language"].get()) != launcher_text.language():
            self.after_idle(self.on_language, self.collect(), self.saved)
        elif hasattr(self, "achievements_box"):
            self.refresh_achievements()

    def _resolutions(self):
        """Output resolutions: the ultrawide ones only with the native renderer."""
        native = RENDERER_PLUGIN.get(self.vars["renderer"].get()) == "native"
        return RESOLUTIONS + (ULTRAWIDE if native else [])

    def _display_changed(self):
        """Keep the output and render resolutions consistent with the renderer and each other."""
        if "render_scale" not in self.combos:
            return  # still building the screen
        # The graphics path is a Xenos setting: only shown with Xenos (grid_remove keeps its place in the grid,
        # so it comes back where it was; its empty row takes no space meanwhile). Anti-aliasing works with both.
        xenos = RENDERER_PLUGIN.get(self.vars["renderer"].get()) == "xenos"
        for key in ("render_path",):
            if key in self.combos:
                for widget in (self.combos[key], self.combo_labels[key]):
                    if xenos:
                        widget.grid()
                    else:
                        widget.grid_remove()
        if not IS_WINDOWS and "upscale" in self.combos:  # Linux: no FidelityFX filters to choose from
            for widget in (self.combos["upscale"], self.combo_labels["upscale"]):
                widget.grid_remove()
        resolutions = self._resolutions()
        self._set_combo_values("resolution", resolutions)
        if self.vars["resolution"].get() not in resolutions:
            self.vars["resolution"].set("1440p")  # an ultrawide output with Xenos: nearest 16:9; re-enters
            return
        resolution = self.vars["resolution"].get()
        self._set_combo_values("render_scale", render_scales(resolution))
        current = self.vars["render_scale"].get()
        wanted = matching_scale(resolution, current)
        if wanted != current:
            self.vars["render_scale"].set(wanted)

    # actions --------------------------------------------------------------
    def collect(self):
        for k, v in self.vars.items():
            self.s[k] = v.get()
        self.s["kbm"] = self.s.get("input_device") == INPUT_DEVICES[1]  # what the rest of the launcher reads
        return self.s

    def reset(self):
        for k, v in self.vars.items():
            if k not in ("render_scale", "launcher_language"):  # Defaults keeps the launcher's language
                v.set(DEFAULTS[k])
        self.vars["render_scale"].set(DEFAULTS["render_scale"])

    def check_achievements(self):
        """On opening: when there's no achievement list yet, or one from before every language was in it, read
        it from the game's default.xex (xex_achievements.py) in a background thread, so the Achievements tab
        doesn't wait for the game's first start. The game rewrites the same list whenever it starts."""
        folder = user_data_dir(self.info) / "achievements"
        try:
            lists = sorted(folder.glob("*-list.json")) if folder.is_dir() else []
            current = bool(lists) and all("names" in a for a in
                                          json.loads(lists[0].read_text(encoding="utf-8")).get("achievements", []))
        except (OSError, ValueError, AttributeError):
            current = False
        xex = Path(self.info["game"]) / "default.xex"
        if current or not xex.is_file():
            return
        result = {}

        def work():
            try:
                result["path"] = xex_achievements.export(xex, user_data_dir(self.info))
            except Exception as e:  # a launcher that can't read it just waits for the game, as before
                result["error"] = e
            result["done"] = True

        threading.Thread(target=work, daemon=True).start()
        self.after(300, self._poll_achievements, result, 100)

    def _poll_achievements(self, result, tries):
        try:
            if not self.winfo_exists():
                return
            if not result.get("done"):
                if tries > 0:
                    self.after(300, self._poll_achievements, result, tries - 1)
                return
            if result.get("path"):
                self.refresh_achievements()
        except tk.TclError:
            pass

    def check_updates(self, manual):
        """Asks GitHub for the newest release in a background thread. On opening (manual=False)
        only a newer release shows; from the button, the status line also says when this is the
        newest or GitHub couldn't be reached."""
        result = {}
        threading.Thread(target=lambda: result.update(found=latest_release(), done=True), daemon=True).start()
        if manual:
            self.status.set(tr("Checking for updates..."))
        self.after(300, self._poll_update, result, manual, 40)

    def _poll_update(self, result, manual, tries):
        # Tk isn't called from the check's thread: the window looks for its answer every 300 ms.
        try:
            if not self.winfo_exists():
                return
            if not result.get("done"):
                if tries > 0:
                    self.after(300, self._poll_update, result, manual, tries - 1)
                elif manual:
                    self.status.set(tr("Couldn't reach GitHub to check for updates."))
                return
            found = result.get("found")
            if found and version_key(found[0]) > version_key(VERSION):
                self._show_update(*found)
                if manual:
                    self.status.set(tr("Version {version} is out: \"Download and update\" above.",
                                       version=found[0].lstrip('vV')))
            elif manual:
                self.status.set(tr("You have the latest version ({version}).", version=VERSION) if found
                                else tr("Couldn't reach GitHub to check for updates."))
        except tk.TclError:  # the view was replaced (reinstall) meanwhile
            pass

    def _show_update(self, tag, url, exe_url=None):
        if self._update_shown:
            return
        self._update_shown = True
        version = tag.lstrip("vV")
        tk.Label(self.update_line, text=tr("Update available: {version} (you have {current}).", version=version,
                                           current=VERSION),
                 font=("Segoe UI", 10, "bold"), fg=ACCENT, bg=BG).pack(side="left")
        if exe_url:
            tk.Button(self.update_line, text=tr("Download and update"), font=("Segoe UI", 10, "bold"), bd=0, padx=8,
                      command=lambda: self.download_update(version, exe_url)).pack(side="left", padx=10)
        self._link_bg(self.update_line, tr("What's new"), lambda: webbrowser.open(url)).pack(side="left")

    def _link_bg(self, parent, text, cmd):
        return self._link(parent, text, cmd)

    def download_update(self, version, exe_url):
        """Downloads the new release's exe beside the launcher's settings, then offers to start it.
        The new launcher opens on the same install and asks for the reinstall (the rebuild
        notice), which also makes it the launcher the desktop shortcut starts."""
        dest = LAUNCHER_HOME / "updates" / (f"BourneConspiracy-{version}.exe" if IS_WINDOWS
                                             else f"BourneConspiracy-{version}")
        state = {"frac": None, "done": False, "error": None}

        def work():
            try:
                download_file(exe_url, dest, lambda f: state.update(frac=f))
            except Exception as e:  # network, disk: reported below
                state["error"] = e
            state["done"] = True

        def poll():
            try:
                if not self.winfo_exists():
                    return
            except tk.TclError:
                return
            if not state["done"]:
                f = state["frac"]
                self.status.set(tr("Downloading {version}...", version=version) +
                                (f" {int(f * 100)}%" if f is not None else ""))
                self.after(250, poll)
                return
            if state["error"]:
                self.status.set(tr("Download failed ({error}). Use \"What's new\" to download it yourself.",
                                   error=state['error']))
                return
            self.status.set(tr("Downloaded {version} to {folder}.", version=version, folder=dest.parent))
            if messagebox.askyesno(tr("Update"), tr("{version} is downloaded.\n\nStart it now? This launcher closes, "
                                                    "and the new one asks you to reinstall the game (pick your disc "
                                                    "image) so the game is rebuilt with the update.",
                                                    version=version)):
                try:
                    if not IS_WINDOWS:
                        dest.chmod(0o755)
                    start_game([str(dest)], dest.parent)
                except OSError as e:
                    messagebox.showerror(tr("Update"), tr("Couldn't start {file}: {error}", file=dest, error=e))
                    return
                self.winfo_toplevel().destroy()

        threading.Thread(target=work, daemon=True).start()
        self.status.set(tr("Downloading {version}...", version=version))
        self.after(250, poll)

    def show_keys(self):
        win = tk.Toplevel(self)
        win.title(tr("Keyboard and mouse"))
        win.configure(bg=BG)
        win.transient(self.winfo_toplevel())
        win.resizable(False, False)
        body = tk.Frame(win, bg=BG)
        body.pack(fill="both", expand=True, padx=22, pady=(16, 8))
        row = 0
        for title, keys in CONTROLS:
            tk.Label(body, text=tr(title), font=("Segoe UI", 11, "bold"), fg=ACCENT, bg=BG, anchor="w"
                     ).grid(row=row, column=0, columnspan=2, sticky="w", pady=(10 if row else 0, 2))
            row += 1
            for key, action in keys:
                tk.Label(body, text=tr(key), font=("Segoe UI", 10, "bold"), fg=FG, bg=BG, anchor="w"
                         ).grid(row=row, column=0, sticky="w", padx=(0, 18))
                tk.Label(body, text=tr(action), font=("Segoe UI", 10), fg=FG, bg=BG, anchor="w"
                         ).grid(row=row, column=1, sticky="w")
                row += 1
        tk.Label(body, text=tr("The layout is fixed: the prompts and the key images in the game are drawn for it."),
                 font=("Segoe UI", 8), fg=DIM, bg=BG, anchor="w").grid(row=row, column=0, columnspan=2,
                                                                       sticky="w", pady=(12, 0))
        foot = tk.Frame(win, bg=BG)
        foot.pack(fill="x", padx=22, pady=(4, 16))
        tk.Button(foot, text=tr("Close"), width=10, command=win.destroy, font=("Segoe UI", 10), bd=0, pady=4
                  ).pack(side="right")
        link_button(foot, tr("Full page with notes"), lambda: webbrowser.open(CONTROLS_URL)).pack(side="left")
        win.bind("<Escape>", lambda e: win.destroy())
        win.grab_set()
        win.focus_set()

    def open_logs(self):
        folder = logs_dir(self.info)
        try:
            open_path(str(folder))
        except OSError as e:
            messagebox.showerror(tr("Open logs"), tr("Couldn't open {folder}: {error}", folder=folder, error=e))

    def open_saves(self):
        folder = user_data_dir(self.info)
        try:
            folder.mkdir(parents=True, exist_ok=True)
            open_path(str(folder))
        except OSError as e:
            messagebox.showerror(tr("Saves"), tr("Couldn't open {folder}: {error}", folder=folder, error=e))

    def backup_saves(self):
        try:
            out = backup_saves(self.info)
        except (OSError, zipfile.BadZipFile) as e:
            messagebox.showerror(tr("Back up saves"), tr("Couldn't back up the saves: {error}", error=e))
            return
        messagebox.showinfo(tr("Back up saves"), tr("Saved {file} to {folder}.\n\nTo restore it, close the game and "
                                                    "unzip it into {saves folder}, replacing the files there.")
                            .replace("{file}", out.name).replace("{folder}", str(out.parent))
                            .replace("{saves folder}", str(out.parent.parent)))
        try:
            open_path(str(out.parent))
        except OSError:
            pass

    def refresh_saves(self):
        """The Saves tab's list: every saved game in the saves folder (list_saves)."""
        root = user_data_dir(self.info)
        self.saves_tree.delete(*self.saves_tree.get_children())
        self.save_paths = {}
        saves = list_saves(root)
        for i, save in enumerate(saves):
            iid = str(i)
            self.save_paths[iid] = save["path"]
            self.saves_tree.insert("", "end", iid=iid, values=(
                save["name"], self._date(time.localtime(save["modified"])) +
                time.strftime("  %H:%M", time.localtime(save["modified"])),
                tr("{n} KB", n=f"{save['size'] / 1024:.0f}"), save["path"].name))
        where = tr("Saves folder: {folder}", folder=root)
        count = (tr("{n} saved game. ", n=1) if len(saves) == 1 else tr("{n} saved games. ", n=len(saves))) if saves \
            else tr("No saved games yet. ")
        self.saves_label.configure(text=count + where + "\n" + tr("Double-click a save to open its folder."))

    @staticmethod
    def _date(t):
        """A day in the launcher's language: 17 Nov 2026 (the month's short name from the translations)."""
        months = tr("Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec").split()
        return f"{t.tm_mday} {months[t.tm_mon - 1]} {t.tm_year}"

    def open_save_folder(self):
        sel = self.saves_tree.selection()
        if sel and sel[0] in self.save_paths:
            try:
                open_path(str(self.save_paths[sel[0]]))
            except OSError as e:
                messagebox.showerror(tr("Saves"), tr("Couldn't open the folder: {error}", error=e))

    def refresh_achievements(self):
        """The Achievements tab: every achievement, unlocked first (newest first), with icon, description,
        gamerscore and when it was unlocked."""
        box = self.achievements_box
        for child in box.winfo_children():
            child.destroy()
        try:
            language = self.vars["language"].get()  # the achievements' names: the game's language
            items = load_achievements(self.info, LANGUAGES.get(language, 1))
        except (OSError, ValueError, KeyError) as e:
            tk.Label(box, text=tr("Couldn't read the achievements: {error}", error=e), font=("Segoe UI", 10), fg=FG, bg=PANEL
                     ).pack(anchor="w")
            return
        if not items:
            tk.Label(box, text=tr("Start the game once (to the title screen) and its achievements appear here."),
                     font=("Segoe UI", 10), fg=FG, bg=PANEL).pack(anchor="w")
            return
        got = [a for a in items if a["unlocked"]]
        score = sum(int(a["gamerscore"]) for a in got)
        total = sum(int(a["gamerscore"]) for a in items)
        tk.Label(box, text=tr("{got} of {total} unlocked    {score} of {max} G", got=len(got), total=len(items),
                              score=score, max=total), font=("Segoe UI", 11, "bold"),
                 fg=FG, bg=PANEL, anchor="w").pack(fill="x", pady=(0, 8))
        outer = tk.Frame(box, bg=PANEL)
        outer.pack(fill="both", expand=True)
        canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0)
        bar = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        rows = tk.Frame(canvas, bg=PANEL)
        rows.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        window = canvas.create_window((0, 0), window=rows, anchor="nw")
        canvas.bind("<Configure>", lambda e: canvas.itemconfigure(window, width=e.width))
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")
        scroll = lambda e: canvas.yview_scroll(-1 * (e.delta // 120), "units")
        canvas.bind("<Enter>", lambda e: canvas.bind_all("<MouseWheel>", scroll))
        canvas.bind("<Leave>", lambda e: canvas.unbind_all("<MouseWheel>"))
        self.achievement_images = []  # keep the icons alive
        items = sorted(got, key=lambda a: time.mktime(a["unlocked"]), reverse=True) + [a for a in items if not a["unlocked"]]
        for i, a in enumerate(items):
            bg = BG if a["unlocked"] else PANEL
            row = tk.Frame(rows, bg=bg, padx=8, pady=6)
            row.pack(fill="x", pady=2)
            icon = None
            if a.get("icon_path") and a["icon_path"].is_file():
                try:
                    icon = tk.PhotoImage(file=str(a["icon_path"]))
                    if icon.width() >= 64:
                        icon = icon.subsample(max(1, icon.width() // 48))
                    self.achievement_images.append(icon)
                except tk.TclError:
                    icon = None
            holder = tk.Frame(row, bg=bg, width=48, height=48)
            holder.grid(row=0, column=0, rowspan=2, padx=(0, 10))
            holder.grid_propagate(False)
            if icon:
                tk.Label(holder, image=icon, bg=bg).place(relx=0.5, rely=0.5, anchor="center")
            dim = not a["unlocked"]
            tk.Label(row, text=a["name"], font=("Segoe UI", 10, "bold"), fg=DIM if dim else FG, bg=bg,
                     anchor="w").grid(row=0, column=1, sticky="w")
            desc = a["description"] if a["unlocked"] or not a.get("locked_description") else a["locked_description"]
            tk.Label(row, text=desc, font=("Segoe UI", 9), fg=DIM, bg=bg, anchor="w", justify="left",
                     wraplength=420).grid(row=1, column=1, sticky="w")
            if a["unlocked"]:
                when = tr("Unlocked {date}", date=self._date(a["unlocked"]))
            else:
                when = tr("Locked")
            tk.Label(row, text=f"{a['gamerscore']} G", font=("Segoe UI", 10, "bold"), fg=ACCENT if a["unlocked"] else DIM,
                     bg=bg).grid(row=0, column=2, sticky="e", padx=(10, 0))
            tk.Label(row, text=when, font=("Segoe UI", 8), fg=DIM, bg=bg).grid(row=1, column=2, sticky="e", padx=(10, 0))
            row.columnconfigure(1, weight=1)

    def crash_report(self):
        try:
            out = save_crash_report(self.info)
        except OSError as e:
            messagebox.showerror(tr("Crash report"), tr("Couldn't save the report: {error}", error=e))
            return
        messagebox.showinfo(tr("Crash report"), tr("Saved {file} to {folder}.\n\nAttach it when you report a problem. "
                                                   "It contains the game log, your launcher settings and any crash "
                                                   "dump (no game files).", file=out.name, folder=out.parent))

    def save(self):
        """Saves the settings without starting the game (Play saves them too)."""
        save_settings(self.collect())
        self.saved = self._snapshot()
        self.status.set(tr("Settings saved."))

    def _snapshot(self) -> str:
        """The settings as they stand, without the open tab (switching tabs isn't a change worth asking about)."""
        return json.dumps({k: v for k, v in self.collect().items() if k != "tab"}, sort_keys=True, default=str)

    def unsaved(self) -> bool:
        return self._snapshot() != getattr(self, "saved", None)

    def confirm_close(self) -> bool:
        """Asks to save changed settings before the launcher closes; False to stay open."""
        if not self.unsaved():
            return True
        answer = messagebox.askyesnocancel(tr("Quit"), tr("Save your changed settings before quitting?"))
        if answer is None:
            return False
        if answer:
            self.save()
        return True

    def quit_launcher(self):
        if self.confirm_close():
            self.winfo_toplevel().destroy()

    def play(self):
        if self.needs_rebuild and not messagebox.askyesno(
                tr("Play"), tr("The game hasn't been rebuilt for {version} yet, so this version's changes aren't "
                               "in it, and its new settings may not work.\n\nPlay anyway? (Reinstall now to rebuild "
                               "it.)", version=VERSION)):
            return
        s = self.collect()
        save_settings(s)
        self.saved = self._snapshot()
        exe, game = Path(self.info["exe"]), Path(self.info["game"])
        try:
            summary = apply_game_config(game, s)
        except PermissionError:
            messagebox.showerror(tr("Play"), tr("Couldn't update the game's config files. Is the game already running?"))
            return
        except Exception as e:  # keep the launcher usable even if config editing fails
            if not messagebox.askyesno(tr("Play"), tr("Couldn't apply the config settings ({error}).\n\nStart the "
                                                      "game with its original config instead?", error=e)):
                return
            summary = tr("config unchanged")
        try:
            apply_intro_skip(game, False)  # undo the removed "skip intro" option, if it was used
        except OSError as e:
            messagebox.showwarning(tr("Play"), tr("Couldn't change the intro movies ({error}). Starting anyway.",
                                                  error=e))
        log = Path(self.info.get("root", exe.parent)) / "game.log"
        args = [str(exe), "--game_data_root", str(game)] + game_args(s, (self.winfo_screenwidth(), self.winfo_screenheight())) + ["--log_file", str(log)]
        args += user_data_args(self.info, exe)
        try:
            if RENDERER_PLUGIN.get(s.get("renderer")) == "native" and not (
                    any(exe.parent.glob("rexgpu-native*.dll")) or any(exe.parent.glob("librexgpu-native*.so"))):
                messagebox.showwarning(tr("Play"), tr("The native renderer isn't installed (this build's SDK wasn't "
                                                      "made with it). Starting with Xenos instead."))
                args[args.index("native")] = "xenos"
            start_game(args, exe.parent, game_env(s))
        except OSError as e:
            messagebox.showerror(tr("Play"), tr("Couldn't start the game: {error}", error=e))
            return
        self.status.set(tr("Started with {summary}.", summary=summary))
        if s["close_on_play"]:
            self.after(800, self.winfo_toplevel().destroy)


class Launcher:
    def __init__(self):
        self.root = make_root(APP_NAME)
        height = min(740, self.root.winfo_screenheight() - 80)
        self.root.geometry(f"980x{height}")
        self.root.minsize(900, 660)
        self.view = None
        s = load_settings()
        launcher_text.set_language(launcher_text.resolve(s.get("launcher_language", "same"),
                                                         s.get("language") if load_install() else None))
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

    def show_settings(self, info, settings=None, saved=None):
        self._swap(SettingsView(self.root, info, on_reinstall=lambda: self.show_install(), on_locate=self.locate,
                                on_language=lambda s, done: self._relabel(info, s, done), settings=settings,
                                saved=saved))

    def _relabel(self, info, s, saved):
        """The launcher in another language: the settings screen drawn again with the settings as they stood."""
        launcher_text.set_language(launcher_text.resolve(s.get("launcher_language", "same"), s.get("language")))
        self.show_settings(info, settings=s, saved=saved)

    def show_install(self, first_time=False, iso="", folder=None):
        info = load_install()
        default = Path(info["root"]) if info and info.get("root") else None
        back = None if first_time else (lambda: self.show_settings(load_install()))
        view = InstallView(self.root, on_installed=self.installed, on_back=back, default_dir=default,
                           on_language=lambda choice, iso, folder: self._relabel_install(first_time, choice, iso,
                                                                                         folder),
                           language_choice=load_settings().get("launcher_language", "same"), iso=iso)
        if folder:
            view.dir_var.set(folder)
        if first_time:
            self._link_locate(view)
        self._swap(view)

    def _relabel_install(self, first_time, choice, iso, folder):
        """The install screen in another language: the choice saved, the screen drawn again with what was typed."""
        s = load_settings()
        s["launcher_language"] = choice
        try:
            save_settings(s)
        except OSError:
            pass
        info = load_install()
        launcher_text.set_language(launcher_text.resolve(choice, s.get("language") if info else None))
        self.show_install(first_time, iso=iso, folder=folder)

    def _link_locate(self, view):
        link_button(view, tr("Already installed? Locate tbc.exe..."), self.locate).pack(anchor="w", padx=28, pady=(8, 0))

    def installed(self, exe, game, root):
        self.show_settings(save_install(exe, game, root))

    def locate(self):
        exe = filedialog.askopenfilename(title=tr("Locate tbc.exe"),
                                         filetypes=[("tbc.exe", "tbc.exe"), (tr("Programs"), "*.exe")] if IS_WINDOWS
                                         else [(EXE_NAME, EXE_NAME), (tr("All files"), "*")])
        if not exe:
            return
        game = filedialog.askdirectory(title=tr("Choose the extracted game folder (the one with default.xex)"))
        if not game:
            return
        if not (Path(game) / "default.xex").is_file():
            messagebox.showerror(tr("Locate"), tr("That folder doesn't contain default.xex."))
            return
        self.show_settings(save_install(exe, game))

    def on_close(self):
        if isinstance(self.view, SettingsView) and not self.view.confirm_close():
            return
        if isinstance(self.view, InstallView) and self.view.busy():
            if not messagebox.askyesno(tr("Install running"), tr("An install is still running. Quit anyway?\nRunning "
                                                                 "the launcher again will pick up where it left off.")):
                return
        self.root.destroy()

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    Launcher().run()
