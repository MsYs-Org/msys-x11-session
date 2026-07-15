from __future__ import annotations

import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "msys_x11_build_package", ROOT / "scripts" / "build_package.py"
)
assert SPEC is not None and SPEC.loader is not None
BUILD_PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD_PACKAGE)


class PackageBuilderTests(unittest.TestCase):
    def test_package_versions_are_locked_to_the_canonical_manifest(self) -> None:
        version = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))[
            "package"
        ]["version"]
        self.assertEqual(version, "0.2.13")
        self.assertIn(
            f"PACKAGE_VERSION := {version}",
            (ROOT / "Makefile").read_text(encoding="utf-8"),
        )
        self.assertIn(
            f'version = "{version}"',
            (ROOT / "pyproject.toml").read_text(encoding="utf-8"),
        )

    def test_archive_contains_only_runtime_tree_and_runs_isolated(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "source"
            fixture.mkdir()
            shutil.copy2(ROOT / "manifest.json", fixture / "manifest.json")
            shutil.copytree(ROOT / "msys_x11_session", fixture / "msys_x11_session")
            (fixture / "scripts").mkdir()
            for name in (
                "msys_window_policy_entry.py",
                "msys_display_session_state.py",
                "msys_x11_policy_provider.sh",
                "msys_hdmi_x11_provider.sh",
                "msys_ch347_x11_provider.sh",
            ):
                shutil.copy2(ROOT / "scripts" / name, fixture / "scripts" / name)
            binary = fixture / "bin" / "msys-x11-policy"
            binary.parent.mkdir()
            binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            binary.chmod(binary.stat().st_mode | stat.S_IXUSR)

            archive = BUILD_PACKAGE.build(fixture)
            self.assertEqual(archive.name, "org.msys.x11.session-0.2.13.tar.gz")
            with tarfile.open(archive, "r:gz") as bundle:
                names = set(bundle.getnames())
                self.assertIn("manifest.json", names)
                self.assertIn("bin/msys-x11-policy", names)
                self.assertIn("msys_x11_session/mipc.py", names)
                self.assertIn("scripts/msys_window_policy_entry.py", names)
                self.assertIn("scripts/msys_ch347_x11_provider.sh", names)
                self.assertFalse(any("__pycache__" in name for name in names))
                installed = Path(temporary) / "installed"
                bundle.extractall(installed)

            env = os.environ.copy()
            env["PYTHONPATH"] = "/must/not/be-used"
            result = subprocess.run(
                [
                    sys.executable,
                    "-I",
                    str(installed / "scripts" / "msys_window_policy_entry.py"),
                    "--check-import",
                ],
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            display_help = subprocess.run(
                [
                    sys.executable,
                    "-I",
                    str(installed / "scripts" / "msys_display_session_state.py"),
                    "--help",
                ],
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
                check=False,
            )
            self.assertEqual(display_help.returncode, 0, display_help.stderr)
            self.assertIn("display-session", display_help.stdout)


if __name__ == "__main__":
    unittest.main()
