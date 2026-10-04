#!/usr/bin/env python3
"""Copy the engine runtime beside Billiards, updating only changed files."""

from pathlib import Path
import shutil
import sys


def copy_changed_files(source: Path, destination: Path) -> bool:
    changed = False
    source_files = {path.relative_to(source) for path in source.rglob("*") if path.is_file()}
    for source_file in source.rglob("*"):
        if not source_file.is_file():
            continue
        destination_file = destination / source_file.relative_to(source)
        destination_file.parent.mkdir(parents=True, exist_ok=True)
        if not destination_file.exists() or source_file.read_bytes() != destination_file.read_bytes():
            shutil.copy2(source_file, destination_file)
            changed = True
    if destination.exists():
        for destination_file in destination.rglob("*"):
            if destination_file.is_file() and destination_file.relative_to(destination) not in source_files:
                destination_file.unlink()
                changed = True
    return changed


def main() -> None:
    runtime_source = Path(sys.argv[1])
    build_directory = Path(sys.argv[2])
    stamp = Path(sys.argv[3])
    if not runtime_source.is_dir():
        raise SystemExit(f"Krisp runtime directory does not exist: {runtime_source}")

    changed = copy_changed_files(runtime_source, build_directory / "krisp-runtime")
    (build_directory / "resources" / "billiards").mkdir(parents=True, exist_ok=True)
    stamp.parent.mkdir(parents=True, exist_ok=True)
    if changed or not stamp.exists():
        stamp.touch()


if __name__ == "__main__":
    main()
