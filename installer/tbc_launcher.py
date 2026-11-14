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

import tbc_config as cfg
from tbc_installer import (ACCENT, APP_NAME, BG, DIM, FG, LAUNCHER_HOME, InstallView, install_info_path,
                           make_root, start_game)

VERSION = "0.2.4-2"
# Update check: the newest release on GitHub, asked once when the launcher opens.
RELEASES_API = "https://api.github.com/repos/thegh0sts/bourne-conspiracy-x360-recomp/releases/latest"
RELEASES_URL = "https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/releases/latest"
# The keyboard-and-mouse layout, explained (the "Key binds" window's full-page link).
CONTROLS_URL = "https://github.com/thegh0sts/bourne-conspiracy-x360-recomp/blob/main/docs/keyboard-and-mouse.md"
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
# The layout is fixed: the button prompts, the key glyphs drawn into the HUD's badges and QTE
# prompts (key_glyphs.cpp), the fight attacks on the mouse buttons and the QTEs on W / A / S / D
# are all set up around it, so it isn't editable. The launcher links to docs/keyboard-and-mouse.md.
# The keyboard-and-mouse layout as the "Key binds" window shows it (docs/keyboard-and-mouse.md
# has the same, with notes).
CONTROLS = [
    ("On foot", [
        ("W / A / S / D", "Move"),
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
        ("Arrow keys", "Turn the camera"),
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
        ("W / S", "Move up and down"),
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


def latest_release(timeout=5):
    """(tag, page URL) of the newest GitHub release, or None if it can't be had."""
    req = urllib.request.Request(RELEASES_API, headers={"Accept": "application/vnd.github+json",
                                                        "User-Agent": f"BourneConspiracyPC-launcher/{VERSION}"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = json.loads(r.read().decode("utf-8"))
        return data["tag_name"], data.get("html_url") or RELEASES_URL
    except Exception:  # offline, rate-limited, blocked: no update notice, nothing else
        return None


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
    "mouse_sensitivity": 100,
    "mouse_look": True,
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
    s.pop("mouse_attacks", None)  # was an option; now always on with keyboard and mouse
    s["mouse_sensitivity"] = min(400, max(10, int(s.get("mouse_sensitivity", 100))))  # slider was 10-1000
    s.pop("aspect", None)  # the old separate "picture shape" setting: now set by the resolution
    if s.get("resolution") not in RESOLUTIONS + ULTRAWIDE:
        s["resolution"] = DEFAULTS["resolution"]
    if is_ultrawide(s["resolution"]) and RENDERER_PLUGIN.get(s.get("renderer")) != "native":
        s["resolution"] = DEFAULTS["resolution"]
    s["render_scale"] = matching_scale(s["resolution"], s["render_scale"])
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


# Mouse look (src/mouse_look.cpp): the camera turns by as much as the mouse moves. The game
# turns the camera through each strategy's orbit behaviours (BourneCamera.ini /
# Xe-BourneCamera.ini): a turn rate at full stick (YawSpeed / PitchSpeed, 65 to 120 degrees a
# second), a ramp that speeds it up after the stick is held for YawRampTime, and a smoother.
# For the mouse: full-stick rates 4x (so fast flicks fit under full stick; mouse_look.cpp's
# --mouse_look_full_rate is 4 x 65), no ramp, and a quarter of the smoothing.
MOUSE_RATE_FACTOR = 4


def _edit_camera_for_mouse(body):
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
            lines[i] = f"{key}={v / 4:g}"
    return "\r\n".join(lines)


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
    fight_mouse = bool(s.get("kbm"))  # with keyboard and mouse, fights are always on the mouse buttons
    mouse_camera = bool(s.get("kbm")) and bool(s.get("mouse_look", True))

    if turn != 100:
        # The camera's top yaw rate (100 in the original): a cap on how fast it can turn,
        # whatever the mode's turn rate, so it scales with the turn speed too.
        edits.append(("Xe-BourneInput.ini", "BourneNative.BnPlayerInput", "MaxYawRate", f"{100 * turn / 100:g}"))

    if not edits and fov == 100 and not look_edit and not fight_mouse and not mouse_camera:
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
    if mouse_camera:
        for short in ("Xe-BourneCamera.ini", "BourneCamera.ini"):
            try:
                i = cfg.find_file(files, short)
            except SystemExit:
                continue
            name, body = files[i]
            files[i] = (name, _edit_camera_for_mouse(body))
    shutil.copy2(toc_bak, toc)  # update the TOC entry starting from the original
    cfg.write(game, files)
    return f"{len(edits) + (fov != 100) + look_edit + fight_mouse + mouse_camera} config change(s)"


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
        # Mouse look: the sensitivity slider's 100% is 2.0 (0.022 degrees a count x 2).
        args.append(f"--mouse_look_sensitivity={2 * int(s.get('mouse_sensitivity', 100)) / 100:.2f}")
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
        self.combos = {}

        head = tk.Frame(self, bg=BG)
        head.pack(fill="x", padx=28, pady=(20, 4))
        tk.Label(head, text=APP_NAME.upper(), font=("Segoe UI", 22, "bold"), fg=ACCENT, bg=BG).pack(side="left")
        tk.Label(head, text=f"Launcher {VERSION}", font=("Segoe UI", 9), fg=DIM, bg=BG).pack(side="right", anchor="s")
        # Filled in by the update check, if a newer release is out.
        self.update_line = tk.Frame(self, bg=BG)
        self.update_line.pack(fill="x", padx=28)
        self._update_shown = False
        self.check_updates(manual=False)

        box = self._section("Game files")
        tk.Label(box, text=f"Installed in {info['game']}", font=("Segoe UI", 9), fg=DIM, bg=PANEL,
                 anchor="w").grid(row=0, column=0, columnspan=2, sticky="w")
        links = tk.Frame(box, bg=PANEL)
        links.grid(row=1, column=0, columnspan=2, sticky="w", pady=(4, 0))
        self._link(links, "Reinstall from a disc image...", on_reinstall).pack(side="left")
        self._link(links, "Use a different install...", on_locate).pack(side="left", padx=14)
        self._link(links, "Open logs", self.open_logs).pack(side="left")
        self._link(links, "Save crash report", self.crash_report).pack(side="left", padx=14)
        self._link(links, "Check for updates", lambda: self.check_updates(manual=True)).pack(side="left")

        box = self._section("Display")
        self._combo(box, 0, "renderer", "Renderer", RENDERERS)
        self._check(box, 1, "fullscreen", "Fullscreen")
        self._combo(box, 2, "resolution", "Output resolution", self._resolutions())
        self._combo(box, 4, "render_scale", "Render resolution (sharper, slower)",
                    render_scales(self.s["resolution"]))
        tk.Label(box, text="Ultrawide outputs are experimental and need the native renderer.",
                 font=("Segoe UI", 8), fg=DIM, bg=PANEL, anchor="w").grid(row=3, column=0, columnspan=2, sticky="w")
        self.vars["renderer"].trace_add("write", lambda *_: self._display_changed())
        self.vars["resolution"].trace_add("write", lambda *_: self._display_changed())
        self._combo(box, 5, "render_path", "Graphics path (Xenos only)", RENDER_PATHS)
        self._combo(box, 6, "upscale", "Upscaling filter", UPSCALE)
        self._combo(box, 7, "aa", "Anti-aliasing (Xenos only)", AA)
        self._combo(box, 8, "fps", "Frame rate", FPS)
        self._checks(box, 9, [("vsync", "VSync (off: may tear, can be faster)"), ("show_fps", "Show FPS counter")])
        self._check(box, 10, "original_pacing", "Original Xbox 360 frame pacing (slower)")
        tk.Label(box, text="Native renderer by the Skullburglar project (github.com/nlwstein/skullburglar, "
                           "BSD 3-Clause).", font=("Segoe UI", 8), fg=DIM, bg=PANEL, anchor="w"
                 ).grid(row=11, column=0, columnspan=2, sticky="w", pady=(4, 0))

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
        self._link(line, "Key binds", self.show_keys).pack(side="left", padx=14)
        self._check(box, 2, "mouse_look", "Mouse turns the camera directly (mouse look)")
        self._slider(box, 3, "mouse_sensitivity", "Mouse sensitivity (%)", 10, 400, 5)
        self._slider(box, 4, "turn_speed", "Stick turn speed (%)", 50, 300, 10)
        tk.Label(box, text="Stick turn speed is for a controller and the arrow keys; with mouse look the "
                           "camera follows the mouse.", font=("Segoe UI", 8), fg=DIM, bg=PANEL, anchor="w"
                 ).grid(row=5, column=0, columnspan=2, sticky="w")

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
        combo = ttk.Combobox(box, textvariable=v, values=values, state="readonly", width=32)
        combo.grid(row=row, column=1, sticky="e", pady=2)
        self.combos[key] = combo

    def _resolutions(self):
        """Output resolutions: the ultrawide ones only with the native renderer."""
        native = RENDERER_PLUGIN.get(self.vars["renderer"].get()) == "native"
        return RESOLUTIONS + (ULTRAWIDE if native else [])

    def _display_changed(self):
        """Keep the output and render resolutions consistent with the renderer and each other."""
        if "render_scale" not in self.combos:
            return  # still building the screen
        resolutions = self._resolutions()
        self.combos["resolution"]["values"] = resolutions
        if self.vars["resolution"].get() not in resolutions:
            self.vars["resolution"].set("1440p")  # an ultrawide output with Xenos: nearest 16:9; re-enters
            return
        resolution = self.vars["resolution"].get()
        self.combos["render_scale"]["values"] = render_scales(resolution)
        current = self.vars["render_scale"].get()
        wanted = matching_scale(resolution, current)
        if wanted != current:
            self.vars["render_scale"].set(wanted)

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
            if k != "render_scale":
                v.set(DEFAULTS[k])
        self.vars["render_scale"].set(DEFAULTS["render_scale"])

    def check_updates(self, manual):
        """Asks GitHub for the newest release in a background thread. On opening (manual=False)
        only a newer release shows; from the button, the status line also says when this is the
        newest or GitHub couldn't be reached."""
        result = {}
        threading.Thread(target=lambda: result.update(found=latest_release(), done=True), daemon=True).start()
        if manual:
            self.status.set("Checking for updates...")
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
                    self.status.set("Couldn't reach GitHub to check for updates.")
                return
            found = result.get("found")
            if found and version_key(found[0]) > version_key(VERSION):
                self._show_update(*found)
                if manual:
                    self.status.set(f"Version {found[0].lstrip('vV')} is out: use the Download link above.")
            elif manual:
                self.status.set(f"You have the latest version ({VERSION})." if found
                                else "Couldn't reach GitHub to check for updates.")
        except tk.TclError:  # the view was replaced (reinstall) meanwhile
            pass

    def _show_update(self, tag, url):
        if self._update_shown:
            return
        self._update_shown = True
        tk.Button(self.update_line, text=f"Update available: {tag.lstrip('vV')} (you have {VERSION}). Download...",
                  command=lambda: webbrowser.open(url), font=("Segoe UI", 10, "bold", "underline"), fg=ACCENT,
                  bg=BG, activebackground=BG, activeforeground=FG, bd=0, cursor="hand2").pack(anchor="w")

    def show_keys(self):
        win = tk.Toplevel(self)
        win.title("Keyboard and mouse")
        win.configure(bg=BG)
        win.transient(self.winfo_toplevel())
        win.resizable(False, False)
        body = tk.Frame(win, bg=BG)
        body.pack(fill="both", expand=True, padx=22, pady=(16, 8))
        row = 0
        for title, keys in CONTROLS:
            tk.Label(body, text=title, font=("Segoe UI", 11, "bold"), fg=ACCENT, bg=BG, anchor="w"
                     ).grid(row=row, column=0, columnspan=2, sticky="w", pady=(10 if row else 0, 2))
            row += 1
            for key, action in keys:
                tk.Label(body, text=key, font=("Segoe UI", 10, "bold"), fg=FG, bg=BG, anchor="w"
                         ).grid(row=row, column=0, sticky="w", padx=(0, 18))
                tk.Label(body, text=action, font=("Segoe UI", 10), fg=FG, bg=BG, anchor="w"
                         ).grid(row=row, column=1, sticky="w")
                row += 1
        tk.Label(body, text="The layout is fixed: the prompts and the key images in the game are drawn for it.",
                 font=("Segoe UI", 8), fg=DIM, bg=BG, anchor="w").grid(row=row, column=0, columnspan=2,
                                                                       sticky="w", pady=(12, 0))
        foot = tk.Frame(win, bg=BG)
        foot.pack(fill="x", padx=22, pady=(4, 16))
        tk.Button(foot, text="Close", width=10, command=win.destroy, font=("Segoe UI", 10), bd=0, pady=4
                  ).pack(side="right")
        tk.Button(foot, text="Full page with notes", command=lambda: webbrowser.open(CONTROLS_URL),
                  font=("Segoe UI", 9, "underline"), fg=DIM, bg=BG, activebackground=BG, activeforeground=ACCENT,
                  bd=0, cursor="hand2").pack(side="left")
        win.bind("<Escape>", lambda e: win.destroy())
        win.grab_set()
        win.focus_set()

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
