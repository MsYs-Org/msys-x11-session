#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SESSION_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
STATE_ENTRY="$SESSION_ROOT/scripts/msys_display_session_state.py"
STATE_NATIVE="$SESSION_ROOT/bin/msys-x11-policy"
DISPLAY_ID="${DISPLAY_ID:-${DISPLAY:-:0}}"
RUN_DIR="${MSYS_HDMI_RUN_DIR:-/tmp/msys-x11-hdmi}"
READY_FILE="${MSYS_X11_READY_FILE:-$RUN_DIR/display-session.json}"
if [ -n "${MSYS_DISPLAY_SESSION_STATE_FILE:-}" ]; then
    SESSION_STATE_FILE="$MSYS_DISPLAY_SESSION_STATE_FILE"
elif [ -n "${MSYS_RUNTIME_DIR:-}" ]; then
    SESSION_STATE_FILE="$MSYS_RUNTIME_DIR/display-session.json"
else
    SESSION_STATE_FILE="$READY_FILE"
fi
SESSION_OWNER_FILE="$SESSION_STATE_FILE.owner"
OWNER_TOKEN="${MSYS_GENERATION:-0}:$$:$(date +%s)"
LOG_FILE="${MSYS_HDMI_LOG_FILE:-$RUN_DIR/xorg.log}"
XORG_BIN="${MSYS_XORG_BINARY:-Xorg}"
XORG_PID=
CLEANED=0

find_session_python()
{
    if [ -n "${MSYS_PYTHON:-}" ] && [ -x "$MSYS_PYTHON" ]; then
        printf '%s\n' "$MSYS_PYTHON"
    elif [ -x /opt/msys-dev/.runtime/python/bin/python3 ]; then
        printf '%s\n' /opt/msys-dev/.runtime/python/bin/python3
    elif command -v python3 >/dev/null 2>&1; then
        command -v python3
    elif command -v python >/dev/null 2>&1; then
        command -v python
    else
        return 1
    fi
}

cleanup()
{
    if [ "$CLEANED" = "1" ]; then
        return
    fi
    CLEANED=1
    if [ -f "$SESSION_OWNER_FILE" ] &&
            [ "$(cat "$SESSION_OWNER_FILE" 2>/dev/null || true)" = "$OWNER_TOKEN" ]; then
        rm -f "$READY_FILE" "$SESSION_STATE_FILE" "$SESSION_OWNER_FILE"
    else
        echo "msys-hdmi-provider: session ownership moved; leaving replacement state"
    fi
    if [ -n "$XORG_PID" ] && kill -0 "$XORG_PID" 2>/dev/null; then
        kill "$XORG_PID" 2>/dev/null || true
        wait "$XORG_PID" 2>/dev/null || true
    fi
}

handle_term()
{
    cleanup
    exit 0
}

trap handle_term INT TERM
trap cleanup EXIT

mkdir -p "$RUN_DIR" "$(dirname -- "$READY_FILE")" "$(dirname -- "$SESSION_STATE_FILE")"
rm -f "$READY_FILE"

if [ -n "${MSYS_XORG_CONFIG:-}" ]; then
    "$XORG_BIN" "$DISPLAY_ID" -config "$MSYS_XORG_CONFIG" -nolisten tcp -noreset >"$LOG_FILE" 2>&1 &
else
    "$XORG_BIN" "$DISPLAY_ID" -nolisten tcp -noreset >"$LOG_FILE" 2>&1 &
fi
XORG_PID=$!
echo "msys-hdmi-provider: Xorg pid=$XORG_PID display=$DISPLAY_ID log=$LOG_FILE"

ready=0
for _ in $(seq 1 200); do
    if ! kill -0 "$XORG_PID" 2>/dev/null; then
        echo "msys-hdmi-provider: Xorg exited before readiness" >&2
        tail -80 "$LOG_FILE" 2>/dev/null || true
        exit 1
    fi
    if DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.05
done

if [ "$ready" != "1" ]; then
    echo "msys-hdmi-provider: X11 display did not become ready: $DISPLAY_ID" >&2
    tail -80 "$LOG_FILE" 2>/dev/null || true
    exit 1
fi

publish_display_state()
{
    local state_file="$1"
    local python
    local native_rc
    if [ -x "$STATE_NATIVE" ]; then
        set +e
        DISPLAY="$DISPLAY_ID" \
        MSYS_DISPLAY_INPUT_MODE="${MSYS_DISPLAY_INPUT_MODE:-none}" \
            "$STATE_NATIVE" --publish-display-session "$state_file" \
            "${MSYS_COMPONENT_ID:-org.msys.x11.session:hdmi-output}"
        native_rc=$?
        set -e
        if [ "$native_rc" = "0" ]; then
            return 0
        fi
        echo "msys-hdmi-provider: native publisher rc=$native_rc; using verified compatibility publisher" >&2
    fi
    python=$(find_session_python) || {
        echo "msys-hdmi-provider: Python runtime unavailable for display-session state" >&2
        return 1
    }
    MSYS_DISPLAY_INPUT_MODE="${MSYS_DISPLAY_INPUT_MODE:-none}" \
        "$python" "$STATE_ENTRY" \
        --display "$DISPLAY_ID" \
        --provider "${MSYS_COMPONENT_ID:-org.msys.x11.session:hdmi-output}" \
        --state-file "$state_file"
}
publish_all_display_state()
{
    local tmp="$SESSION_OWNER_FILE.$$.tmp"
    printf '%s\n' "$OWNER_TOKEN" > "$tmp"
    mv -f "$tmp" "$SESSION_OWNER_FILE"
    publish_display_state "$SESSION_STATE_FILE"
    if [ "$READY_FILE" != "$SESSION_STATE_FILE" ]; then
        publish_display_state "$READY_FILE"
    fi
}
publish_all_display_state
echo "msys-hdmi-provider: ready state=$SESSION_STATE_FILE ready-file=$READY_FILE"

display_fail_checks=0
state_refresh_checks=0
while kill -0 "$XORG_PID" 2>/dev/null; do
    if DISPLAY="$DISPLAY_ID" xdpyinfo >/dev/null 2>&1; then
        display_fail_checks=0
        state_refresh_checks=$((state_refresh_checks + 1))
        if [ "$state_refresh_checks" -ge 5 ]; then
            publish_all_display_state >/dev/null
            state_refresh_checks=0
        fi
    else
        display_fail_checks=$((display_fail_checks + 1))
        if [ "$display_fail_checks" -ge 5 ]; then
            echo "msys-hdmi-provider: X11 display became unavailable: $DISPLAY_ID" >&2
            exit 1
        fi
    fi
    sleep 2
done

set +e
wait "$XORG_PID"
rc=$?
set -e
XORG_PID=
exit "$rc"
