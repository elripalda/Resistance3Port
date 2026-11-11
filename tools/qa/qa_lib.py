"""Shared helpers for the QA capture / soak tools (tools\\qa). Python 3.8+, needs psutil, numpy, Pillow.

Game profiles live in tools\\qa\\qa_game.json (one per repo). Never imported by the game.
"""
import ctypes
import ctypes.wintypes as wt
import datetime
import glob
import json
import os
import re
import shutil
import subprocess
import time

import psutil

QA_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(QA_DIR, "..", ".."))


def rel(p):
    try:
        return os.path.relpath(p, ROOT)
    except ValueError:
        return p


def log(msg):
    print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


def load_profile(path=None):
    with open(path or os.path.join(QA_DIR, "qa_game.json"), encoding="utf-8-sig") as f:
        return json.load(f)


def expand(s, run_dir=""):
    return s.replace("{root}", ROOT).replace("{run}", run_dir).replace("%~dp0", ROOT + "\\")


def bat_env(path):
    """set "K=V" lines of a .bat (settings.bat / run.bat). Conditional lines are taken as-is."""
    env = {}
    p = os.path.join(ROOT, path)
    if not os.path.isfile(p):
        return env
    for line in open(p, encoding="utf-8", errors="ignore"):
        m = re.match(r'^\s*set\s+"(\w+)=(.*)"\s*$', line, re.I)
        if m:
            env[m.group(1)] = expand(m.group(2))
    return env


def pick_exe(profile, override=None):
    if override:
        return os.path.abspath(override)
    for e in profile["exe"]:
        p = os.path.join(ROOT, e)
        if os.path.isfile(p):
            return p
    raise SystemExit(f"no game exe found (tried {profile['exe']})")


# ------------------------------------------------------------------ the game process
class Game:
    def __init__(self, profile, run_dir, exe=None, extra_env=None):
        self.p = profile
        self.run_dir = run_dir
        self.exe = pick_exe(profile, exe)
        self.cmd_file = os.path.join(run_dir, "dbg.cmd")
        env = dict(os.environ)
        for b in profile.get("env_from", []):
            env.update(bat_env(b))
        for k, v in profile.get("env", {}).items():
            env[k] = expand(str(v), run_dir)
        env["PS3_DEBUG"] = self.cmd_file
        for k, v in (extra_env or {}).items():
            env[k] = expand(str(v), run_dir)
        self.env = env
        self.proc = None
        self.game_pid = None
        self.t0 = None

    def start(self):
        os.makedirs(self.run_dir, exist_ok=True)
        for f in glob.glob(self.cmd_file + "*"):
            os.remove(f)
        with open(os.path.join(self.run_dir, "env.txt"), "w") as f:
            f.write(f"exe={self.exe}\n")
            for k in sorted(self.env):
                if k in self.p.get("env", {}) or k.startswith(("R2_", "R3_", "RFOM_", "RSX_", "PS3_", "CHILD_", "SPURS_", "PORT_")):
                    f.write(f"{k}={self.env[k]}\n")
        out = open(os.path.join(self.run_dir, "out.log"), "wb")
        err = out if self.p.get("merge_stderr") else open(os.path.join(self.run_dir, "run.log"), "wb")
        args = [self.exe] + [expand(a, self.run_dir) for a in self.p.get("args", [])]
        self.proc = subprocess.Popen(args, cwd=ROOT, env=self.env, stdout=out, stderr=err)
        self.t0 = time.time()
        log(f"started {rel(self.exe)} pid {self.proc.pid}")
        return self.proc.pid

    def game_process(self):
        """psutil.Process of the real game (R1: the ResistanceFoMGame.exe child, re-found after a hand-over)."""
        name = self.p.get("game_process")
        if not name:
            try:
                return psutil.Process(self.proc.pid)
            except psutil.Error:
                return None
        if self.game_pid:
            try:
                q = psutil.Process(self.game_pid)
                if q.is_running() and q.status() != psutil.STATUS_ZOMBIE:
                    return q
            except psutil.Error:
                pass
        best = None
        for q in psutil.process_iter(["name", "create_time"]):
            if (q.info["name"] or "").lower() == name.lower() and (q.info["create_time"] or 0) >= self.t0 - 2:
                if best is None or q.info["create_time"] > best.info["create_time"]:
                    best = q
        self.game_pid = best.pid if best else None
        return best

    def alive(self):
        if self.p.get("game_process"):
            if self.game_process():
                return True
            return self.proc.poll() is None  # front end still running (between hand-overs)
        return self.proc.poll() is None

    def exit_code(self):
        return self.proc.poll()

    def console(self, command, settle=0.4):
        with open(self.cmd_file, "w") as f:
            f.write(command)
        time.sleep(settle)
        with open(self.cmd_file, "w") as f:
            f.write("")

    def stop(self):
        procs = []
        for pid in {self.proc.pid if self.proc else None, self.game_pid}:
            if not pid:
                continue
            try:
                q = psutil.Process(pid)
                procs += [q] + q.children(recursive=True)
            except psutil.Error:
                pass
        for q in procs:
            try:
                q.kill()
            except psutil.Error:
                pass
        psutil.wait_procs(procs, timeout=10)
        log("game stopped")

    def log_text(self, name):
        p = os.path.join(self.run_dir, name)
        try:
            with open(p, "rb") as f:
                return f.read().decode("utf-8", "ignore")
        except OSError:
            return ""


# ------------------------------------------------------------------ window capture fallback
user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32


def game_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            r = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            found.append((r.right * r.bottom, hwnd))
        return True

    user32.EnumWindows(cb, 0)
    return max(found)[1] if found else None


def is_hung(pid):
    h = game_window(pid)
    return bool(h and user32.IsHungAppWindow(h))


def window_capture(pid, path):
    """PrintWindow(PW_RENDERFULLCONTENT): the window's own content, also when covered (DWM-composed swapchains)."""
    from PIL import Image
    hwnd = game_window(pid)
    if not hwnd:
        return False
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    if w <= 0 or h <= 0:
        return False
    wdc = user32.GetDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(wdc)
    bm = gdi32.CreateCompatibleBitmap(wdc, w, h)
    gdi32.SelectObject(mdc, bm)
    ok = user32.PrintWindow(hwnd, mdc, 1 | 2)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]
    bih = BIH(ctypes.sizeof(BIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    got = gdi32.GetDIBits(mdc, bm, 0, h, buf, ctypes.byref(bih), 0)
    gdi32.DeleteObject(bm)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, wdc)
    if not ok or got != h:
        return False
    Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1).save(path)
    return True


# ------------------------------------------------------------------ frame capture
def renderer_capture_armed(game):
    return "[r2qa] frame capture armed" in game.log_text("run.log") + game.log_text("out.log")


def capture(game, cap_dir, scene, frames=6, every=20, mode=None, timeout=60):
    """Writes <cap_dir>\\<scene>_NN.png. every = flips between frames (60 fps based for the non-renderer modes)."""
    os.makedirs(cap_dir, exist_ok=True)
    for old in glob.glob(os.path.join(cap_dir, f"{scene}_[0-9][0-9].png")):
        os.remove(old)
    mode = mode or game.p.get("capture", "window")
    if mode == "renderer" and not renderer_capture_armed(game):
        log("renderer capture not armed (exe without R2_QA_CAPTURE support?) - falling back to window capture")
        mode = "window"
    if mode == "renderer":
        done = os.path.join(cap_dir, scene + ".done")
        if os.path.exists(done):
            os.remove(done)
        with open(os.path.join(cap_dir, "capture.req.tmp"), "w") as f:
            f.write(f"{frames} {scene} {every}\n")
        os.replace(os.path.join(cap_dir, "capture.req.tmp"), os.path.join(cap_dir, "capture.req"))
        t = time.time()
        while time.time() - t < timeout:
            if os.path.exists(done):
                os.remove(done)
                n = len(glob.glob(os.path.join(cap_dir, f"{scene}_[0-9][0-9].png")))
                log(f"captured {scene}: {n} frame(s) (renderer)")
                return n
            if not game.alive():
                break
            time.sleep(0.1)
        log(f"capture {scene}: renderer capture timed out (window minimized / not flipping?)")
        return 0
    gp = game.game_process()
    if not gp:
        return 0
    n = 0
    if mode == "f12":
        shots = os.path.join(ROOT, game.p.get("screenshot_dir", "screenshots"))
        pattern = os.path.join(shots, game.p.get("screenshot_glob", "*.png"))
        for i in range(frames):
            before = set(glob.glob(pattern))
            hwnd = game_window(gp.pid)
            if not hwnd:
                break
            user32.PostMessageW(hwnd, 0x0100, 0x7B, 0x00580001)            # WM_KEYDOWN VK_F12
            user32.PostMessageW(hwnd, 0x0101, 0x7B, 0xC0580001)            # WM_KEYUP
            t = time.time()
            new = []
            while time.time() - t < 10 and not new:
                time.sleep(0.1)
                new = sorted(set(glob.glob(pattern)) - before)
            if not new:
                log("F12 screenshot did not appear; window capture for the rest")
                mode = "window"
                break
            last = -1
            while True:  # written on a worker thread: wait until the size settles
                time.sleep(0.15)
                sz = os.path.getsize(new[-1])
                if sz == last and sz > 0:
                    break
                last = sz
            shutil.move(new[-1], os.path.join(cap_dir, f"{scene}_{i:02d}.png"))
            n += 1
            time.sleep(max(0.0, every / 60.0 - 0.3))
        if mode == "f12":
            log(f"captured {scene}: {n} frame(s) (F12 back-buffer readback)")
            return n
    for i in range(n, frames):
        if window_capture(gp.pid, os.path.join(cap_dir, f"{scene}_{i:02d}.png")):
            n += 1
        time.sleep(every / 60.0)
    log(f"captured {scene}: {n} frame(s) (window)")
    return n


# ------------------------------------------------------------------ scripted steps
def run_steps(game, steps, cap_dir=None, scenes=None, on_tick=None):
    """steps: "wait S" | "pad BITS WAIT [lx ly rx ry ms]" | "console WAIT text..." | "capture NAME [frames] [every]".
    Returns False if the game died."""
    for st in steps:
        parts = st.split()
        if not parts or parts[0].startswith("#"):
            continue
        op = parts[0]
        if op == "wait":
            dur = float(parts[1])
        elif op == "pad":
            stick = parts[3:8] if len(parts) >= 8 else ["128", "128", "128", "128", "200"]
            game.console(f"pad {parts[1]} {' '.join(stick)}")
            dur = float(parts[2])
            log(f"pad {parts[1]} ({' '.join(stick)}), wait {dur:g}s")
        elif op == "console":
            game.console(" ".join(parts[2:]))
            dur = float(parts[1])
            log(f"console: {' '.join(parts[2:])}")
        elif op == "capture":
            if cap_dir is not None and (not scenes or parts[1] in scenes):
                capture(game, cap_dir, parts[1], int(parts[2]) if len(parts) > 2 else 6,
                        int(parts[3]) if len(parts) > 3 else 20)
            dur = 0
        else:
            raise SystemExit(f"unknown step '{st}'")
        end = time.time() + dur
        while time.time() < end:
            if not game.alive():
                log("game exited during the scripted steps")
                return False
            if on_tick:
                on_tick()
            time.sleep(min(0.5, max(0.0, end - time.time())))
    return True


def stamp():
    return datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
