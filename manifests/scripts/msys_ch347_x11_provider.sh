#!/usr/bin/env sh
set -eu

# Development compatibility for a nested manifest. Packaged manifests are
# promoted to package root and call the canonical scripts/ path directly.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
exec "$ROOT/scripts/msys_ch347_x11_provider.sh" "$@"
