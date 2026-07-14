#!/usr/bin/env python3
"""Source/install-safe entry point for the display-session state publisher."""

from __future__ import annotations

import sys
from pathlib import Path


PACKAGE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE_ROOT))

from msys_x11_session.display_session import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())
