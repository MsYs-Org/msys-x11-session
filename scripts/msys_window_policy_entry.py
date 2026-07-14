#!/usr/bin/env python3
"""Installed/source-tree entry point with a package-local import root."""

from __future__ import annotations

import os
import sys
from pathlib import Path


PACKAGE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE_ROOT))

from msys_x11_session.window_policy_agent import main  # noqa: E402


if __name__ == "__main__":
    if "--native-policy" in sys.argv[1:]:
        position = sys.argv.index("--native-policy")
        try:
            native_policy = sys.argv[position + 1]
        except IndexError:
            print("msys-window-policy-entry: --native-policy requires a path", file=sys.stderr)
            raise SystemExit(2)
        if not native_policy or native_policy.startswith("-"):
            print("msys-window-policy-entry: invalid native policy path", file=sys.stderr)
            raise SystemExit(2)
        os.environ["MSYS_X11_POLICY_BINARY"] = native_policy
        del sys.argv[position : position + 2]
    if "--check-import" in sys.argv[1:]:
        print(f"msys-window-policy-entry: ok root={PACKAGE_ROOT}")
        raise SystemExit(0)
    raise SystemExit(main())
