# Billiards

Billiards is a standalone Krisp application. It consumes the static `krisp/0.1.0` Conan package; day-to-day engine development uses Conan editable mode so engine edits do not create a new cached package version.

## Local development

Requirements: Conan 2, Meson, Ninja, a C++20 compiler, and the Linux Vulkan and FFmpeg development packages used by Krisp.

Register the local engine checkout once (replace the path as needed):

```sh
conan editable add /path/to/krisp --name=krisp --version=0.1.0
```

Install the application graph and configure its Debug build:

```sh
conan install . -pr=conan_clang_profile --build=missing --no-remote
meson setup build/debug \
  --native-file build/conan/conan_meson_native.ini \
  --native-file build/conan/krisp-runtime.ini \
  --buildtype=debug
```

Build the engine first so its runtime assets are current, then build Billiards:

```sh
meson compile -C /path/to/krisp/build/debug -j 6 krisp
meson compile -C build/debug -j 6
```

The Billiards build deploys the runtime beside the executable at `build/debug/krisp-runtime`. Run `build/debug/billiards` from any working directory. App-specific resources can be placed under `build/debug/resources/billiards`; writable config uses the platform's XDG config directory, and saves live under `$XDG_DATA_HOME/krisp/billiards/saves` (default `~/.local/share/krisp/billiards/saves`).

After intentional engine changes are ready for a package checkpoint, build `krisp/0.1.0` from the engine checkout. To consume that package instead of the editable checkout, remove the editable mapping with `conan editable remove --refs=krisp/0.1.0` and reinstall this application graph. Normal source edits should keep the same `0.1.0` reference and use editable mode.

For a cached checkpoint, follow the package command in Krisp’s `docs/CONAN.md`, remove the editable registration, reinstall the app dependencies, and reconfigure Meson with `--reconfigure --clearcache`. Package revisions are checkpoints, not a version bump for every edit.

## Scene loading

Billiards saves the object IDs for its playing surface, table, rails, cue, and
balls, along with each ball's pocketed state and reset position. Scene loading
uses those IDs after Krisp restores objects and physics, so edited display names
do not affect gameplay state. Physics bodies, velocities, and sleep state are
restored by Krisp before the app refreshes its transient input and UI state.
Reset Rack restores the saved initial positions.

When switching between editable and cached Krisp, clear Meson's dependency cache:

```sh
meson setup build/debug --reconfigure --clearcache \
  --native-file build/conan/conan_meson_native.ini \
  --native-file build/conan/krisp-runtime.ini \
  --buildtype=debug -Db_ndebug=false
```

## Regression check

With Xvfb, PyYAML, and the X11/XTest runtime libraries installed, run:

```sh
xvfb-run -a python scripts/smoke_test.py build/debug/billiards
```

This drives the real app through save, repeated load, gameplay ticks, and clean
shutdown. It uses temporary XDG directories and detects exceptions even when the
engine catches them and exits with status zero.
