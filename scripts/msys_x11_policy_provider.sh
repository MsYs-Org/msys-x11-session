#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PACKAGE_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROOT="${MSYS_PACKAGE_ROOT:-${MSYS_X11_SESSION_ROOT:-$PACKAGE_ROOT}}"
BIN="${MSYS_X11_POLICY_BINARY:-$ROOT/bin/msys-x11-policy}"

if [ ! -x "$BIN" ]; then
    echo "msys-x11-policy-provider: prebuilt binary unavailable: $BIN" >&2
    echo "run 'make all' for a source checkout or 'make package' before install" >&2
    exit 127
fi

exec "$BIN"
