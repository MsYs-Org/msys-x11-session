#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DISPLAY_ID=${1:-:98}
XVFB_PID=
POLICY_PID=

case "$DISPLAY_ID" in
    :0|:24)
        if [ "${MSYS_ALLOW_ACTIVE_DISPLAY:-0}" != "1" ]; then
            echo "probe-native-rss: refusing active display $DISPLAY_ID; use an isolated display" >&2
            exit 2
        fi
        ;;
esac

cleanup()
{
    if [ -n "$POLICY_PID" ]; then
        kill "$POLICY_PID" 2>/dev/null || true
        wait "$POLICY_PID" 2>/dev/null || true
    fi
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if ! DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1; then
    Xvfb "$DISPLAY_ID" -screen 0 320x480x24 -nolisten tcp >"/tmp/msys-x11-rss-xvfb.$$.log" 2>&1 &
    XVFB_PID=$!
    attempts=0
    while ! DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 100 ] || ! kill -0 "$XVFB_PID" 2>/dev/null; then
            echo "probe-native-rss: isolated Xvfb did not become ready" >&2
            exit 1
        fi
        sleep 0.05
    done
fi

DISPLAY="$DISPLAY_ID" "$ROOT/bin/msys-x11-policy" >"/tmp/msys-x11-rss-policy.$$.log" 2>&1 &
POLICY_PID=$!
sleep 0.3
kill -0 "$POLICY_PID"

awk '
    /^VmRSS:/ {rss=$2}
    /^VmHWM:/ {hwm=$2}
    /^VmSize:/ {vsz=$2}
    END {printf "native-policy pid=%d rss_kib=%d hwm_kib=%d vmsize_kib=%d\n", pid, rss, hwm, vsz}
' pid="$POLICY_PID" "/proc/$POLICY_PID/status"

if command -v pmap >/dev/null 2>&1; then
    pmap -x "$POLICY_PID" | tail -1 || true
fi
