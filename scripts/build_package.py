#!/usr/bin/env python3
"""Build the self-contained org.msys.x11.session package archive."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import tarfile
import tempfile
from pathlib import Path
from typing import Any


PACKAGE_ID = "org.msys.x11.session"


def _copy_package(root: Path, staging: Path) -> dict[str, Any]:
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8-sig"))
    package = manifest.get("package", {})
    if manifest.get("schema") != "msys.manifest.v1" or package.get("id") != PACKAGE_ID:
        raise ValueError(f"manifest must describe {PACKAGE_ID}")
    binary = root / "bin" / "msys-x11-policy"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise FileNotFoundError(f"prebuilt native policy is missing: {binary}; run make all")

    staging.mkdir(parents=True)
    shutil.copy2(root / "manifest.json", staging / "manifest.json")
    shutil.copytree(
        root / "msys_x11_session",
        staging / "msys_x11_session",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )
    (staging / "bin").mkdir()
    shutil.copy2(binary, staging / "bin" / binary.name)
    (staging / "scripts").mkdir()
    for name in (
        "msys_window_policy_entry.py",
        "msys_display_session_state.py",
        "msys_x11_policy_provider.sh",
        "msys_hdmi_x11_provider.sh",
        "msys_ch347_x11_provider.sh",
    ):
        shutil.copy2(root / "scripts" / name, staging / "scripts" / name)

    for component in manifest.get("components", []):
        for argument in component.get("exec", []):
            if isinstance(argument, str) and argument.startswith("@package/"):
                target = staging / argument.removeprefix("@package/")
                if not target.exists():
                    raise ValueError(
                        f"component {component.get('id')} references missing package path {argument}"
                    )
    return manifest


def build(root: Path, output: Path | None = None) -> Path:
    root = root.resolve()
    (root / "build").mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="msys-x11-package-", dir=root / "build") as temporary:
        staging = Path(temporary) / "root"
        manifest = _copy_package(root, staging)
        version = str(manifest["package"]["version"])
        archive = output or root / "dist" / f"{PACKAGE_ID}-{version}.tar.gz"
        archive = archive.resolve()
        archive.parent.mkdir(parents=True, exist_ok=True)
        temporary_archive = archive.with_name(f".{archive.name}.{os.getpid()}.tmp")
        try:
            with tarfile.open(temporary_archive, "w:gz", format=tarfile.PAX_FORMAT) as bundle:
                for child in sorted(staging.iterdir(), key=lambda value: value.name):
                    bundle.add(child, arcname=child.name, recursive=True)
            os.replace(temporary_archive, archive)
        except Exception:
            try:
                temporary_archive.unlink()
            except OSError:
                pass
            raise
    return archive


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    (args.root / "build").mkdir(parents=True, exist_ok=True)
    archive = build(args.root, args.output)
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
