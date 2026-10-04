"""Exercise repeated scene loading against the real app under xvfb-run."""

import ctypes
import ctypes.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


executable = Path(sys.argv[1]).resolve()
x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XKeysymToKeycode.restype = ctypes.c_uint
x11.XFlush.argtypes = [ctypes.c_void_p]
x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
display = x11.XOpenDisplay(None)
if not display:
    raise SystemExit("No X display; run this test with xvfb-run -a")


def key(symbol: int) -> None:
    code = x11.XKeysymToKeycode(display, symbol)
    xtst.XTestFakeKeyEvent(display, code, 1, 0)
    xtst.XTestFakeKeyEvent(display, code, 0, 0)
    x11.XFlush(display)


with tempfile.TemporaryDirectory(prefix="billiards-smoke-") as temporary:
    directory = Path(temporary)
    environment = dict(os.environ, XDG_DATA_HOME=str(directory / "data"),
                       XDG_CONFIG_HOME=str(directory / "config"))
    with (directory / "app.log").open("w+") as log:
        app = subprocess.Popen([str(executable)], cwd=directory, env=environment,
                               stdout=log, stderr=subprocess.STDOUT)
        scene = directory / "data/krisp/billiards/saves/quicksave/scene.yaml"
        try:
            def save() -> None:
                prior = scene.stat().st_mtime_ns if scene.exists() else 0
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    if app.poll() is not None:
                        raise RuntimeError("App exited before processing the save")
                    key(0xFFC2)  # F5; also confirms earlier F9 has completed.
                    time.sleep(1)
                    if scene.exists() and scene.stat().st_mtime_ns != prior:
                        return
                raise RuntimeError("Timed out waiting for scene save")

            time.sleep(3)
            save()
            for _ in range(2):
                key(0xFFC6)  # F9
                save()
                time.sleep(1)  # Exercise gameplay ticks with restored bodies.
            key(0xFF1B)  # Escape
            app.wait(timeout=15)
            log.seek(0)
            output = log.read()
            if app.returncode != 0 or "Exception Thrown!" in output:
                raise RuntimeError("App failed during scene load or shutdown")
            print("PASS: scene save, repeated load, gameplay ticks, clean shutdown")
        except Exception:
            log.seek(0)
            print(log.read()[-2000:], file=sys.stderr)
            raise
        finally:
            if app.poll() is None:
                app.terminate()
                try:
                    app.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    app.kill()
                    app.wait()
x11.XCloseDisplay(display)
