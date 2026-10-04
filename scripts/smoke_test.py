"""Exercise repeated scene loading against the real app under xvfb-run."""

import ctypes
import ctypes.util
import os
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time

import yaml


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
            document = yaml.safe_load(scene.read_text())
            app_state = document["application"]
            cue_id = app_state["balls"][0]["id"]
            pocketed_id = app_state["balls"][1]["id"]
            for obj in document["objects"]:
                obj["name"] = "Renamed object " + str(obj["id"])
            bodies = {body["entity_id"]: body for body in document["ecs"]["physics_system"]["bodies"]}
            bodies[cue_id]["linear_velocity"] = {"x": 0.65, "y": 0.0, "z": 0.0}
            bodies[cue_id]["active"] = True
            bodies[pocketed_id]["enabled"] = False
            bodies[pocketed_id]["active"] = False
            app_state["balls"][1]["pocketed"] = True
            document["engine"]["paused"] = True
            scene.write_text(yaml.safe_dump(document, sort_keys=False))

            key(0xFFC6)  # Load the edited scene while paused.
            save()
            loaded = yaml.safe_load(scene.read_text())
            assert all(obj["name"].startswith("Renamed object ") for obj in loaded["objects"])
            assert loaded["application"]["balls"][1]["pocketed"] is True
            loaded_bodies = {body["entity_id"]: body for body in loaded["ecs"]["physics_system"]["bodies"]}
            assert math.isclose(loaded_bodies[cue_id]["linear_velocity"]["x"], 0.65, abs_tol=1e-6)
            assert loaded_bodies[pocketed_id]["enabled"] is False

            loaded["engine"]["paused"] = False
            scene.write_text(yaml.safe_dump(loaded, sort_keys=False))
            key(0xFFC6)
            time.sleep(1)
            save()
            advanced = yaml.safe_load(scene.read_text())
            advanced_transforms = {entry["entity_id"]: entry for entry in advanced["ecs"]["transformation_system"]}
            loaded_transforms = {entry["entity_id"]: entry for entry in loaded["ecs"]["transformation_system"]}
            advanced_x = advanced_transforms[cue_id]["local_transform"]["column_3"]["x"]
            loaded_x = loaded_transforms[cue_id]["local_transform"]["column_3"]["x"]
            assert advanced_x > loaded_x
            key(0xFFC6)
            save()
            key(0xFF1B)  # Escape
            app.wait(timeout=15)
            log.seek(0)
            output = log.read()
            if app.returncode != 0 or "Exception Thrown!" in output:
                raise RuntimeError("App failed during scene load or shutdown")
            print("PASS: explicit app IDs, renamed objects, moving and pocketed bodies, repeated load")
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
