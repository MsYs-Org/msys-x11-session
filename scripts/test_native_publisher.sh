#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DISPLAY_ID=${1:-:109}
TEMPORARY=${TMPDIR:-/tmp}/msys-native-publisher.$$
XVFB_PID=

cleanup()
{
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
    rm -rf "$TEMPORARY"
}
trap cleanup EXIT INT TERM
mkdir -p "$TEMPORARY"

Xvfb "$DISPLAY_ID" -screen 0 320x480x24 -nolisten tcp >"$TEMPORARY/xvfb.log" 2>&1 &
XVFB_PID=$!
ready=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.05
done
if [ "$ready" != "1" ]; then
    echo "test-native-publisher: Xvfb did not become ready" >&2
    exit 1
fi

DISPLAY="$DISPLAY_ID" MSYS_GENERATION=12 MSYS_DISPLAY_INPUT_MODE=none \
    "$ROOT/bin/msys-x11-policy" --publish-display-session \
    "$TEMPORARY/display-session.json" org.msys.test:display >/dev/null

test -s "$TEMPORARY/display-session.json"
grep -q '"schema":"msys.display-session.v1"' "$TEMPORARY/display-session.json"
grep -q '"width":320' "$TEMPORARY/display-session.json"
grep -q '"height":480' "$TEMPORARY/display-session.json"
grep -q '"generation":12' "$TEMPORARY/display-session.json"
grep -q '"matrix":null' "$TEMPORARY/display-session.json"
echo "native display-session publisher passed display=$DISPLAY_ID"
