#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

if [ "$(uname -m)" != "aarch64" ]; then
    echo "build-aarch64-j1: expected an aarch64 build host, got $(uname -m)" >&2
    exit 2
fi

make -C "$ROOT" -j1 clean all
file "$ROOT/bin/msys-x11-policy"
case "$(file -b "$ROOT/bin/msys-x11-policy")" in
    *aarch64*|*ARM\ aarch64*) ;;
    *) echo "build-aarch64-j1: output is not an aarch64 ELF" >&2; exit 1 ;;
esac
size "$ROOT/bin/msys-x11-policy" 2>/dev/null || true
