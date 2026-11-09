"""
The Bourne Conspiracy (Xbox 360) - PC recompilation installer.

Builds the game on the player's own PC from their own disc image:
  1. reads the Xbox 360 ISO (XDVDFS) and extracts the game files
  2. checks default.xex is the right game (title ID 565507D4)
  3. finds or installs Visual Studio Build Tools (C++, Clang, CMake)
  4. downloads the ReXGlue SDK
  5. copies in the bundled project files and builds tbc.exe
  6. creates a desktop shortcut

No game code or data ships with this installer.
Only the Python standard library is used, so it packages with PyInstaller.
"""

import json
import os
import queue
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import traceback
import urllib.request
import zipfile
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP_NAME = "The Bourne Conspiracy"
TITLE_ID = 0x565507D4
SDK_VERSION = "0.10.1"
SDK_REPO = "rexglue/rexglue-sdk"
VS_BOOTSTRAPPER = "https://aka.ms/vs/17/release/vs_buildtools.exe"
PRESET = "win-amd64-relwithdebinfo"
MIN_FREE_GB = 25
DEFAULT_DIR = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "BourneConspiracyPC"
LAUNCHER_EXE_NAME = "BourneConspiracy"

VS_COMPONENTS = [
    "Microsoft.VisualStudio.Workload.VCTools",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
    "Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset",
    "Microsoft.VisualStudio.Component.VC.CMake.Project",
]
# Required on an existing install for us to reuse it.
VS_REQUIRED = [
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
    "Microsoft.VisualStudio.Component.VC.CMake.Project",
]

NO_WINDOW = 0x08000000 if os.name == "nt" else 0


class InstallError(Exception):
    """An error with a message meant for the player."""


def resource_dir() -> Path:
    return Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))


def payload_dir() -> Path:
    return resource_dir() / "payload"


# --------------------------------------------------------------------------
# XDVDFS (Xbox disc filesystem) reader
# --------------------------------------------------------------------------

SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Where the game partition starts inside an ISO: plain/rebuilt, XGD2, XGD3, XGD1.
PARTITION_OFFSETS = [0, 0xFD90000, 0x2080000, 0x18300000]


class XisoReader:
    def __init__(self, path: Path):
        self.f = open(path, "rb")
        self.base = None
        for off in PARTITION_OFFSETS:
            self.f.seek(off + 32 * SECTOR)
            if self.f.read(20) == XDVDFS_MAGIC:
                self.base = off
                root_sector, root_size = struct.unpack("<II", self.f.read(8))
                self.root = (root_sector, root_size)
                break
        if self.base is None:
            self.f.close()
            raise InstallError(
                "This doesn't look like an Xbox 360 disc image.\n"
                "Choose the .iso file you made from your game disc.")

    def close(self):
        self.f.close()

    def _read(self, sector: int, size: int) -> bytes:
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def walk(self, sector=None, size=None, prefix=""):
        """Yield (relative_path, is_dir, start_sector, file_size)."""
        if sector is None:
            sector, size = self.root
        if size == 0:
            return
        table = self._read(sector, size)
        seen = set()
        stack = [0]
        out = []
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(table):
                continue
            seen.add(off)
            left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue  # padding
            name = table[off + 14: off + 14 + nlen].decode("latin-1")
            out.append((name, attr, start, fsize))
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)
        for name, attr, start, fsize in sorted(out):
            rel = f"{prefix}{name}"
            if attr & 0x10:
                yield rel, True, start, fsize
                yield from self.walk(start, fsize, rel + "/")
            else:
                yield rel, False, start, fsize

    def extract(self, dest: Path, progress, skip=("$SystemUpdate",)):
        entries = [e for e in self.walk() if e[0].split("/")[0] not in skip]
        total = sum(e[3] for e in entries if not e[1]) or 1
        done = 0
        for rel, is_dir, start, size in entries:
            target = dest / rel
            if is_dir:
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            self.f.seek(self.base + start * SECTOR)
            with open(target, "wb") as out:
                left = size
                while left:
                    chunk = self.f.read(min(left, 8 << 20))
                    if not chunk:
                        raise InstallError(f"The disc image ends early while reading {rel}.\n"
                                           "It may be incomplete or damaged.")
                    out.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
                    progress(done / total, rel)
        return len(entries)


def xex_title_id(path: Path) -> int:
    with open(path, "rb") as f:
        head = f.read(0x8000)
    if head[:4] != b"XEX2":
        raise InstallError("default.xex on this disc isn't a valid Xbox 360 executable.")
    count = struct.unpack(">I", head[0x14:0x18])[0]
    for i in range(count):
        key, val = struct.unpack(">II", head[24 + i * 8: 32 + i * 8])
        if key == 0x40006:
            return struct.unpack(">I", head[val + 12: val + 16])[0]
    raise InstallError("Couldn't read the title ID from default.xex.")


# --------------------------------------------------------------------------
# Tools: Visual Studio Build Tools, ReXGlue SDK
# --------------------------------------------------------------------------

def vswhere() -> Path:
    return Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"


def find_vs():
    exe = vswhere()
    if not exe.exists():
        return None
    args = [str(exe), "-latest", "-products", "*", "-property", "installationPath", "-requires", *VS_REQUIRED]
    try:
        out = subprocess.run(args, capture_output=True, text=True, creationflags=NO_WINDOW).stdout.strip()
    except OSError:
        return None
    if not out:
        return None
    vcvars = Path(out.splitlines()[0]) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    return vcvars if vcvars.exists() else None


def http_get(url: str, accept="application/octet-stream") -> urllib.request.addinfourl:
    req = urllib.request.Request(url, headers={"User-Agent": "tbc-recomp-installer", "Accept": accept})
    return urllib.request.urlopen(req, timeout=60)


def download(url: str, dest: Path, progress):
    with http_get(url) as r:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        with open(dest, "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                if total:
                    progress(done / total)


def find_sdk_asset():
    api = f"https://api.github.com/repos/{SDK_REPO}/releases"
    releases = []
    for tag in (f"v{SDK_VERSION}", SDK_VERSION):
        try:
            with http_get(f"{api}/tags/{tag}", "application/vnd.github+json") as r:
                releases = [json.load(r)]
                break
        except Exception:
            continue
    if not releases:
        with http_get(f"{api}?per_page=100", "application/vnd.github+json") as r:
            releases = [x for x in json.load(r) if SDK_VERSION in (x.get("tag_name", "") + x.get("name", ""))]
    for rel in releases:
        for a in rel.get("assets", []):
            n = a["name"].lower()
            if n.endswith(".zip") and "win" in n and ("amd64" in n or "x64" in n):
                return a["browser_download_url"], a["name"]
    raise InstallError(f"Couldn't find the ReXGlue SDK {SDK_VERSION} for Windows on GitHub.")


def find_sdk_root(folder: Path):
    for cfg in folder.rglob("rexglue-config.cmake"):
        # .../<root>/lib/cmake/rexglue/rexglue-config.cmake
        return cfg.parents[3]
    for cfg in folder.rglob("rexglueConfig.cmake"):
        return cfg.parents[3]
    return None


# --------------------------------------------------------------------------
# The install job (runs on a worker thread)
# --------------------------------------------------------------------------

class Installer:
    def __init__(self, iso: Path, root: Path, allow_downloads: bool, ui):
        self.iso, self.root, self.allow, self.ui = iso, root, allow_downloads, ui
        self.game = root / "game"
        self.project = root / "project"
        self.sdk_dir = root / "rexglue-sdk"
        self.log_path = root / "install.log"
        self.exe = self.project / "out" / "build" / PRESET / "tbc.exe"

    # helpers ------------------------------------------------------------
    def log(self, msg):
        line = time.strftime("[%H:%M:%S] ") + msg
        with open(self.log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        self.ui.put(("log", line))

    def status(self, msg, frac=None):
        self.ui.put(("status", msg, frac))

    def stage(self, start, end):
        return lambda f, *a: self.ui.put(("progress", start + (end - start) * max(0.0, min(1.0, f))))

    def run_logged(self, cmd, progress=None, cwd=None):
        self.log("> " + cmd)
        env = dict(os.environ)
        env.pop("REXSDK_DIR", None)
        p = subprocess.Popen(cmd, shell=True, cwd=cwd, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, errors="replace",
                             creationflags=NO_WINDOW)
        tail = []
        for line in p.stdout:
            line = line.rstrip()
            if not line:
                continue
            self.log(line)
            tail = (tail + [line])[-25:]
            m = re.match(r"\[(\d+)/(\d+)\]", line)
            if m and progress:
                progress(int(m.group(1)) / int(m.group(2)))
        if p.wait() != 0:
            raise InstallError("A build step failed. Click \"Open log\" for details.\n\nLast output:\n"
                               + "\n".join(tail[-8:]))

    # steps --------------------------------------------------------------
    def check_space(self):
        self.root.mkdir(parents=True, exist_ok=True)
        free = shutil.disk_usage(self.root).free / 1e9
        self.log(f"Free space at install location: {free:.1f} GB")
        if free < MIN_FREE_GB:
            raise InstallError(f"Not enough free space. The install needs about {MIN_FREE_GB} GB, "
                               f"and the chosen drive has {free:.0f} GB free.")

    def extract_game(self):
        xex = self.game / "default.xex"
        if xex.exists() and (self.game / "BourneGame").exists():
            try:
                if xex_title_id(xex) == TITLE_ID:
                    self.log("Game files already extracted, skipping.")
                    return
            except InstallError:
                pass
        self.status("Reading your disc image...")
        reader = XisoReader(self.iso)
        try:
            names = {e[0].lower() for e in reader.walk() if "/" not in e[0]}
            if "default.xex" not in names:
                raise InstallError("This disc image has no default.xex. Is it an Xbox 360 game disc?")
            if self.game.exists():
                shutil.rmtree(self.game)
            self.status("Extracting game files...")
            prog = self.stage(0.02, 0.30)
            last = [0.0]

            def p(frac, name):
                if frac - last[0] > 0.002:
                    last[0] = frac
                    prog(frac)
                    self.status(f"Extracting game files... {frac * 100:.0f}%")
            n = reader.extract(self.game, p)
            self.log(f"Extracted {n} entries to {self.game}")
        finally:
            reader.close()
        tid = xex_title_id(xex)
        if tid != TITLE_ID:
            raise InstallError(f"This disc image is a different game (title ID {tid:08X}).\n"
                               f"This installer needs {APP_NAME} (title ID {TITLE_ID:08X}).")
        self.log("Title ID OK: 565507D4")

    def ensure_vs(self):
        self.status("Looking for C++ build tools...")
        vcvars = find_vs()
        if vcvars:
            self.log(f"Using build tools: {vcvars}")
            return vcvars
        if not self.allow:
            raise InstallError("The C++ build tools (Visual Studio Build Tools with Clang) aren't installed.\n"
                               "Tick \"Allow required components to be downloaded and installed\" and try again.")
        self.status("Downloading Visual Studio Build Tools installer...")
        boot = self.root / "vs_buildtools.exe"
        download(VS_BOOTSTRAPPER, boot, self.stage(0.30, 0.31))
        self.status("Installing Visual Studio Build Tools (several GB, this can take 10-30 minutes)...")
        self.log("Windows will ask for administrator permission.")
        args = ["--quiet", "--wait", "--norestart", "--nocache", "--includeRecommended"]
        for c in VS_COMPONENTS:
            args += ["--add", c]
        ps = ("$p = Start-Process -FilePath '{}' -ArgumentList '{}' -Verb RunAs -Wait -PassThru; exit $p.ExitCode"
              .format(str(boot).replace("'", "''"), " ".join(args)))
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], creationflags=NO_WINDOW)
        self.log(f"Build Tools installer exit code: {r.returncode}")
        if r.returncode not in (0, 3010):
            raise InstallError("Installing Visual Studio Build Tools failed or was cancelled "
                               f"(code {r.returncode}).")
        vcvars = find_vs()
        if not vcvars:
            raise InstallError("Build Tools finished installing but couldn't be found. "
                               "Restart your PC and run the installer again.")
        return vcvars

    def ensure_sdk(self):
        root = find_sdk_root(self.sdk_dir) if self.sdk_dir.exists() else None
        if root:
            self.log(f"ReXGlue SDK already present: {root}")
            return root
        if not self.allow:
            raise InstallError("The ReXGlue SDK needs to be downloaded.\n"
                               "Tick \"Allow required components to be downloaded and installed\" and try again.")
        self.status(f"Downloading ReXGlue SDK {SDK_VERSION}...")
        url, name = find_sdk_asset()
        self.log(f"SDK download: {url}")
        z = self.root / name
        download(url, z, self.stage(0.40, 0.45))
        with zipfile.ZipFile(z) as zf:
            zf.extractall(self.sdk_dir)
        z.unlink(missing_ok=True)
        root = find_sdk_root(self.sdk_dir)
        if not root:
            raise InstallError("The downloaded ReXGlue SDK doesn't have the expected layout.")
        self.log(f"ReXGlue SDK: {root}")
        return root

    def setup_project(self):
        self.status("Preparing project files...")
        src = payload_dir() / "project"
        if not src.exists():
            raise InstallError("Installer is missing its project files (payload). Re-download the installer.")
        self.project.mkdir(parents=True, exist_ok=True)
        for item in src.rglob("*"):
            if item.is_file():
                dst = self.project / item.relative_to(src)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(item, dst)
        # Point the manifest at the extracted game.
        man = self.project / "tbc_manifest.toml"
        text = man.read_text(encoding="utf-8")
        text = re.sub(r'(?m)^game_root\s*=.*$', 'game_root = "../game"', text)
        text = re.sub(r'(?m)^file_path\s*=.*$', 'file_path = "../game/default.xex"', text)
        man.write_text(text, encoding="utf-8")
        self.log("Project files copied; manifest points at ../game")

    def build(self, vcvars: Path, sdk: Path):
        prefix = str(sdk).replace("\\", "/")
        base = f'call "{vcvars}" >nul && cd /d "{self.project}" && '
        # Code generation must run before CMake configures: the project only adds the
        # generated sources to the build if generated/default/sources.cmake already exists.
        rexglue = sdk / "bin" / "rexglue.exe"
        if not rexglue.exists():
            raise InstallError("The ReXGlue SDK is missing bin\\rexglue.exe.")
        self.status("Recompiling the game code (a few minutes, the bar won't move much)...")
        self.ui.put(("progress", 0.49))
        self.run_logged(base + f'"{rexglue}" codegen tbc_manifest.toml')
        if not (self.project / "generated" / "default" / "sources.cmake").exists():
            raise InstallError("Code generation finished but produced no sources. Click \"Open log\" for details.")
        self.ui.put(("progress", 0.55))
        # Always (re)configure, so a build configured before codegen picks up the sources.
        self.status("Configuring the build...")
        self.run_logged(base + f'cmake --preset {PRESET} -DCMAKE_PREFIX_PATH="{prefix}"')
        self.status("Building the game (compiling: this takes a while)...")
        prog = self.stage(0.56, 0.98)

        def p(frac):
            prog(frac)
            self.status(f"Building the game... {frac * 100:.0f}%")
        self.run_logged(base + f'cmake --build out\\build\\{PRESET}', p)
        if not self.exe.exists():
            raise InstallError("The build finished but tbc.exe wasn't produced. Click \"Open log\" for details.")
        plugin = self.exe.parent / "rexgpu-xenosrd.dll"
        if not plugin.exists():
            for cand in sdk.rglob("rexgpu-xenosrd.dll"):
                shutil.copy2(cand, plugin)
                self.log(f"Copied graphics plugin from {cand}")
                break

    def make_launchers(self):
        bat = self.root / "Play.bat"
        bat.write_text(
            "@echo off\r\n"
            f'start "" /d "{self.exe.parent}" "{self.exe}" --game_data_root "{self.game}" '
            f'--gpu_plugin xenos --log_file "{self.root / "game.log"}"\r\n', encoding="utf-8")
        icon = self.root / "icon.ico"
        src_icon = resource_dir() / "icon.ico"
        if src_icon.exists():
            shutil.copy2(src_icon, icon)
        # Remember the install so the launcher can find it.
        info = {"root": str(self.root), "exe": str(self.exe), "game": str(self.game)}
        LAUNCHER_HOME.mkdir(parents=True, exist_ok=True)
        (LAUNCHER_HOME / "launcher.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
        # When running as the packaged launcher, keep a copy next to the game and point the shortcut at it,
        # so settings are applied every time. Otherwise the shortcut starts the game directly.
        target, args, wd = self.exe, f'--game_data_root "{self.game}" --gpu_plugin xenos', self.exe.parent
        if getattr(sys, "frozen", False):
            launcher = self.root / f"{LAUNCHER_EXE_NAME}.exe"
            try:
                if Path(sys.executable).resolve() != launcher.resolve():
                    shutil.copy2(sys.executable, launcher)
                target, args, wd = launcher, "", self.root
            except OSError as e:
                self.log(f"Couldn't copy the launcher next to the game: {e}")
        desktop = Path(os.environ.get("USERPROFILE", str(Path.home()))) / "Desktop"
        lnk = desktop / f"{APP_NAME}.lnk"
        q = lambda p: str(p).replace("'", "''")
        ps = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{lnk}');"
              "$s.TargetPath='{exe}';$s.Arguments='{args}';"
              "$s.WorkingDirectory='{wd}';{icon}$s.Save()").format(
            lnk=q(lnk), exe=q(target), args=q(args), wd=q(wd),
            icon=f"$s.IconLocation='{q(icon)},0';" if icon.exists() else "")
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], creationflags=NO_WINDOW)
        self.log("Desktop shortcut created." if r.returncode == 0 else "Couldn't create desktop shortcut.")

    def run(self):
        try:
            self.root.mkdir(parents=True, exist_ok=True)
            self.log(f"--- Install started. ISO: {self.iso}  Location: {self.root}")
            self.check_space()
            self.extract_game()
            vcvars = self.ensure_vs()
            self.ui.put(("progress", 0.40))
            sdk = self.ensure_sdk()
            self.setup_project()
            self.ui.put(("progress", 0.48))
            self.build(vcvars, sdk)
            self.make_launchers()
            self.ui.put(("progress", 1.0))
            self.log("--- Install finished.")
            self.ui.put(("done", str(self.exe), str(self.game)))
        except InstallError as e:
            self.log("ERROR: " + str(e))
            self.ui.put(("error", str(e)))
        except Exception as e:  # unexpected: keep the traceback in the log
            self.log("UNEXPECTED ERROR:\n" + traceback.format_exc())
            self.ui.put(("error", f"Something unexpected went wrong: {e}\nClick \"Open log\" for details."))


# --------------------------------------------------------------------------
# UI
# --------------------------------------------------------------------------

BG, FG, DIM, ACCENT, FIELD = "#13161c", "#e8e8e8", "#9aa3ad", "#c8102e", "#ffffff"


LAUNCHER_HOME = DEFAULT_DIR.parent / "BourneConspiracyPC"


def install_info_path() -> Path:
    """Where the launcher remembers the install (always in %LOCALAPPDATA%)."""
    return LAUNCHER_HOME / "launcher.json"


def setup_style(widget):
    style = ttk.Style(widget)
    style.theme_use("clam")
    style.configure("Bar.Horizontal.TProgressbar", troughcolor="#2a2f38", background=ACCENT,
                    bordercolor=BG, lightcolor=ACCENT, darkcolor=ACCENT)


class InstallView(tk.Frame):
    """The install screen. on_installed(exe, game, root) is called after a successful install."""

    def __init__(self, master, on_installed=None, on_back=None, default_dir=None):
        super().__init__(master, bg=BG)
        self.on_installed, self.on_back = on_installed, on_back
        self.q = queue.Queue()
        self.worker = None
        self.exe = self.game = None

        pad = dict(padx=28)
        tk.Label(self, text=APP_NAME.upper(), font=("Segoe UI", 26, "bold"), fg=ACCENT, bg=BG
                 ).pack(anchor="w", pady=(22, 0), **pad)
        tk.Label(self, text="Your disc. One installer. Ready to play.", font=("Segoe UI", 12),
                 fg=DIM, bg=BG).pack(anchor="w", **pad)

        self.iso_var = tk.StringVar()
        self.dir_var = tk.StringVar(value=str(default_dir or DEFAULT_DIR))
        self._row("1  Choose your game disc image", self.iso_var, "Choose ISO...", self.pick_iso)
        self._row("2  Install location", self.dir_var, "Change...", self.pick_dir)

        tk.Label(self, text=("The installer reads your ISO and builds the game on this PC. First setup may "
                             "download several GB and take a while. Windows may ask for administrator "
                             "permission."), font=("Segoe UI", 9), fg=DIM, bg=BG, wraplength=690,
                 justify="left").pack(anchor="w", pady=(14, 4), **pad)
        self.allow_var = tk.BooleanVar(value=False)
        tk.Checkbutton(self, text="Allow required components to be downloaded and installed "
                                  "(Visual Studio Build Tools, ReXGlue SDK)",
                       variable=self.allow_var, font=("Segoe UI", 10), fg=FG, bg=BG,
                       selectcolor=BG, activebackground=BG, activeforeground=FG
                       ).pack(anchor="w", **pad)

        self.status_var = tk.StringVar(value="Choose your ISO, then click Install.")
        tk.Label(self, textvariable=self.status_var, font=("Segoe UI", 11, "bold"), fg=FG, bg=BG,
                 wraplength=690, justify="left").pack(anchor="w", pady=(16, 6), **pad)
        self.bar = ttk.Progressbar(self, style="Bar.Horizontal.TProgressbar", maximum=1.0)
        self.bar.pack(fill="x", **pad)

        self.details_open = False
        self.det_btn = tk.Button(self, text="+ Installation details", command=self.toggle_details,
                                 font=("Segoe UI", 10), fg=FG, bg=BG, bd=0, activebackground=BG,
                                 activeforeground=FG, cursor="hand2")
        self.det_btn.pack(anchor="w", pady=(10, 0), **pad)
        self.details = tk.Text(self, height=10, bg="#0b0d11", fg=DIM, font=("Consolas", 9), bd=0,
                               wrap="none", state="disabled")

        btns = tk.Frame(self, bg=BG)
        btns.pack(side="bottom", fill="x", pady=22, **pad)
        self.go_btn = tk.Button(btns, text="Install", width=14, command=self.start,
                                font=("Segoe UI", 12, "bold"), bg=ACCENT, fg="white",
                                activebackground="#e0203f", activeforeground="white", bd=0, pady=8)
        self.go_btn.pack(side="right")
        tk.Button(btns, text="Open log", width=12, command=self.open_log, font=("Segoe UI", 12),
                  bd=0, pady=8).pack(side="right", padx=10)
        if on_back:
            self.back_btn = tk.Button(btns, text="Back", width=10, command=on_back, font=("Segoe UI", 12),
                                      bd=0, pady=8)
            self.back_btn.pack(side="left")

        self.after(100, self.poll)

    def busy(self):
        return bool(self.worker and self.worker.is_alive())

    def _row(self, label, var, btn, cmd):
        tk.Label(self, text=label, font=("Segoe UI", 12, "bold"), fg=FG, bg=BG
                 ).pack(anchor="w", padx=28, pady=(18, 4))
        f = tk.Frame(self, bg=BG)
        f.pack(fill="x", padx=28)
        tk.Entry(f, textvariable=var, font=("Segoe UI", 11), relief="flat", bg=FIELD
                 ).pack(side="left", fill="x", expand=True, ipady=6)
        tk.Button(f, text=btn, command=cmd, font=("Segoe UI", 11), width=12, bd=0, pady=5
                  ).pack(side="left", padx=(10, 0))

    def pick_iso(self):
        p = filedialog.askopenfilename(title="Choose your game disc image",
                                       filetypes=[("Disc images", "*.iso"), ("All files", "*.*")])
        if p:
            self.iso_var.set(p)

    def pick_dir(self):
        p = filedialog.askdirectory(title="Choose install location")
        if p:
            self.dir_var.set(str(Path(p) / "BourneConspiracyPC"))

    def toggle_details(self):
        self.details_open = not self.details_open
        if self.details_open:
            self.details.pack(fill="both", expand=True, padx=28, pady=(6, 0))
            self.det_btn.config(text="- Installation details")
        else:
            self.details.pack_forget()
            self.det_btn.config(text="+ Installation details")

    def open_log(self):
        log = Path(self.dir_var.get()) / "install.log"
        if log.exists():
            os.startfile(log)
        else:
            messagebox.showinfo("Log", "There's no install log yet.")

    def start(self):
        if self.exe:
            if self.on_installed:
                return self.on_installed(self.exe, self.game, self.root_dir)
            return subprocess.Popen([self.exe, "--game_data_root", self.game, "--gpu_plugin", "xenos"],
                                    cwd=str(Path(self.exe).parent))
        iso = Path(self.iso_var.get().strip('" '))
        if not iso.is_file():
            messagebox.showerror("Choose ISO", "Choose your game's .iso file first.")
            return
        root = Path(self.dir_var.get().strip('" '))
        bad = sorted(set(str(root)) & set("[]*?;#$%!&^`~'"))
        if bad or not str(root).isascii():
            messagebox.showerror(
                "Install location",
                "Please choose an install location without these characters: "
                + (" ".join(bad) if bad else "non-English letters") + "\n\n"
                "The build tools treat them as special characters and the build fails.\n"
                f"For example: {DEFAULT_DIR}")
            return
        self.root_dir = root
        self.go_btn.config(state="disabled", text="Installing...")
        if self.on_back:
            self.back_btn.config(state="disabled")
        self.bar["value"] = 0
        self.worker = threading.Thread(target=Installer(iso, root, self.allow_var.get(), self.q).run,
                                       daemon=True)
        self.worker.start()

    def poll(self):
        try:
            while True:
                msg = self.q.get_nowait()
                kind = msg[0]
                if kind == "log":
                    self.details.config(state="normal")
                    self.details.insert("end", msg[1] + "\n")
                    lines = int(self.details.index("end-1c").split(".")[0])
                    if lines > 3000:
                        self.details.delete("1.0", f"{lines - 3000}.0")
                    self.details.see("end")
                    self.details.config(state="disabled")
                elif kind == "status":
                    self.status_var.set(msg[1])
                elif kind == "progress":
                    self.bar["value"] = msg[1]
                elif kind == "done":
                    self.exe, self.game = msg[1], msg[2]
                    self.status_var.set("Installed! A desktop shortcut was created.")
                    self.go_btn.config(state="normal", text="Continue" if self.on_installed else "Play")
                    if self.on_back:
                        self.back_btn.config(state="normal")
                elif kind == "error":
                    self.status_var.set("Install stopped. See the message, or click \"Open log\".")
                    self.go_btn.config(state="normal", text="Install")
                    if self.on_back:
                        self.back_btn.config(state="normal")
                    messagebox.showerror("Install stopped", msg[1])
        except queue.Empty:
            pass
        self.after(100, self.poll)


def make_root(title):
    root = tk.Tk()
    root.title(title)
    root.configure(bg=BG)
    icon = resource_dir() / "icon.ico"
    if icon.exists():
        try:
            root.iconbitmap(default=str(icon))
        except tk.TclError:
            pass
    setup_style(root)
    return root


if __name__ == "__main__":
    r = make_root(f"{APP_NAME} - Install & Play")
    r.geometry("760x640")
    v = InstallView(r)
    v.pack(fill="both", expand=True)
    r.mainloop()
