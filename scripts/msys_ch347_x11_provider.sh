#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SESSION_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
STATE_ENTRY="$SESSION_ROOT/scripts/msys_display_session_state.py"
STATE_NATIVE="$SESSION_ROOT/bin/msys-x11-policy"
if [ -n "${MSYS_X11DISPLAY_ROOT:-}" ]; then
    X11DISPLAY_ROOT="$MSYS_X11DISPLAY_ROOT"
elif [ -n "${MSYS_PACKAGE_ROOT:-}" ] && [ -d "$MSYS_PACKAGE_ROOT/files/x11display" ]; then
    X11DISPLAY_ROOT="$MSYS_PACKAGE_ROOT/files/x11display"
elif [ -d "$SESSION_ROOT/files/x11display" ]; then
    X11DISPLAY_ROOT="$SESSION_ROOT/files/x11display"
else
    # Development compatibility only. A production OpenStick driver package
    # carries this tree under files/x11display and never reaches this fallback.
    X11DISPLAY_ROOT=/root/x11display
fi
START_SCRIPT="${CH347_START_SCRIPT:-$X11DISPLAY_ROOT/scripts/start_ch347_dirty_usb_x11.sh}"
STOP_SCRIPT="${CH347_STOP_SCRIPT:-$X11DISPLAY_ROOT/scripts/stop_ch347_dirty_usb_x11.sh}"
RUN_DIR="${RUN_DIR:-/tmp/ch347_dirty_usb_x11}"
PID_FILE="$RUN_DIR/pids"
LOG_FILE="$RUN_DIR/live.log"
READY_FILE="${MSYS_X11_READY_FILE:-$RUN_DIR/msys.ready}"
if [ -n "${MSYS_DISPLAY_SESSION_STATE_FILE:-}" ]; then
    SESSION_STATE_FILE="$MSYS_DISPLAY_SESSION_STATE_FILE"
elif [ -n "${MSYS_RUNTIME_DIR:-}" ]; then
    SESSION_STATE_FILE="$MSYS_RUNTIME_DIR/display-session.json"
else
    SESSION_STATE_FILE="$READY_FILE"
fi
SESSION_OWNER_FILE="$SESSION_STATE_FILE.owner"
OWNER_FILE="$RUN_DIR/msys.provider.owner"
OWNER_TOKEN="${MSYS_GENERATION:-0}:$$:$(date +%s)"
TOUCH_MODE_FILE="${CH347_TOUCH_MODE_FILE:-$RUN_DIR/touch_mode}"
EFFECTIVE_INPUT_MODE=ch347-direct
CLEANED=0
DISPLAY_READY=0
PUBLISHED_DISPLAY_SIGNATURE=""
PUBLISHED_INPUT_MODE=""

prepare_mutable_config()
{
    local state_root="${MSYS_APP_STATE_DIR:-}"
    local source
    local target
    local tmp
    [ -n "$state_root" ] || return 0
    mkdir -p "$state_root/ch347"
    if [ -z "${CH347_FPS_FILE:-}" ]; then
        source="$X11DISPLAY_ROOT/ch347/fps.env"
        target="$state_root/ch347/fps.env"
        if [ ! -e "$target" ] && [ -f "$source" ]; then
            tmp="$target.$$.tmp"
            cp "$source" "$tmp"
            mv -f "$tmp" "$target"
        fi
        export CH347_FPS_FILE="$target"
    fi
    if [ -z "${CH347_TOUCH_CAL_FILE:-}" ]; then
        source="$X11DISPLAY_ROOT/ch347/touch_calibration.env"
        target="$state_root/ch347/touch_calibration.env"
        if [ ! -e "$target" ] && [ -f "$source" ]; then
            tmp="$target.$$.tmp"
            cp "$source" "$tmp"
            mv -f "$tmp" "$target"
        fi
        export CH347_TOUCH_CAL_FILE="$target"
    fi
}

migrate_legacy_idle_capture_default()
{
    local fps_file="${CH347_FPS_FILE:-}"
    local tmp

    # Preserve a user-selected non-default profile, but migrate the old
    # factory 60/60/1 heartbeat to event-driven capture.  Callers that truly
    # need an idle frame can retain it with MSYS_CH347_KEEP_IDLE_FPS=1.
    [ "${MSYS_CH347_KEEP_IDLE_FPS:-0}" = "1" ] && return 0
    [ -n "$fps_file" ] && [ -f "$fps_file" ] || return 0
    grep -qx 'FPS=60' "$fps_file" || return 0
    grep -qx 'XCAP_MAX_FPS=60' "$fps_file" || return 0
    grep -qx 'XCAP_IDLE_FPS=1' "$fps_file" || return 0
    tmp="$fps_file.$$.tmp"
    sed 's/^XCAP_IDLE_FPS=1$/XCAP_IDLE_FPS=0/' "$fps_file" > "$tmp"
    mv -f "$tmp" "$fps_file"
    echo "msys-ch347-provider: migrated legacy idle capture heartbeat to 0 fps"
}

load_touch_calibration()
{
    local calibration="${CH347_TOUCH_CAL_FILE:-$X11DISPLAY_ROOT/ch347/touch_calibration.env}"
    if [ "${CH347_TOUCH:-0}" = "1" ] &&
            [ "${CH347_TOUCH_CALIBRATE:-0}" != "1" ] &&
            [ -f "$calibration" ] && [ -z "${CH347_TOUCH_X_MIN+x}" ]; then
        # Source once in the provider so the sink and the display-session
        # publisher receive the exact same swap/invert/calibration values.
        set -a
        # shellcheck disable=SC1090
        . "$calibration"
        set +a
        echo "msys-ch347-provider: loaded touch calibration=$calibration"
    fi
}

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

publish_state_file()
{
    local state_file="$1"
    local python
    local native_rc
    if [ -x "$STATE_NATIVE" ]; then
        set +e
        DISPLAY="${DISPLAY_ID:-${DISPLAY:-:24}}" \
        MSYS_DISPLAY_INPUT_MODE="$EFFECTIVE_INPUT_MODE" \
            "$STATE_NATIVE" --publish-display-session "$state_file" \
            "${MSYS_COMPONENT_ID:-org.msys.openstick.ch347:x11-spi-touch-output}"
        native_rc=$?
        set -e
        if [ "$native_rc" = "0" ]; then
            return 0
        fi
        echo "msys-ch347-provider: native publisher rc=$native_rc; using verified compatibility publisher" >&2
    fi
    python=$(find_session_python) || {
        echo "msys-ch347-provider: Python runtime unavailable for display-session state" >&2
        return 1
    }
    [ -f "$STATE_ENTRY" ] || {
        echo "msys-ch347-provider: display-session publisher missing: $STATE_ENTRY" >&2
        return 1
    }
    MSYS_DISPLAY_INPUT_MODE="$EFFECTIVE_INPUT_MODE" \
        "$python" "$STATE_ENTRY" \
        --display "${DISPLAY_ID:-${DISPLAY:-:24}}" \
        --provider "${MSYS_COMPONENT_ID:-org.msys.openstick.ch347:x11-spi-touch-output}" \
        --state-file "$state_file"
}

claim_session_state()
{
    local tmp="$SESSION_OWNER_FILE.$$.tmp"
    mkdir -p "$(dirname -- "$SESSION_STATE_FILE")"
    printf '%s\n' "$OWNER_TOKEN" > "$tmp"
    mv -f "$tmp" "$SESSION_OWNER_FILE"
}

owns_session_state()
{
    [ -f "$SESSION_OWNER_FILE" ] && [ "$(cat "$SESSION_OWNER_FILE" 2>/dev/null || true)" = "$OWNER_TOKEN" ]
}

publish_display_state()
{
    claim_session_state
    publish_state_file "$SESSION_STATE_FILE"
    if [ "$READY_FILE" != "$SESSION_STATE_FILE" ]; then
        publish_state_file "$READY_FILE"
    fi
}

display_signature()
{
    # Do not use the display-session JSON itself as a liveness heartbeat: its
    # atomic replace is intentionally observed by the window policy.  Probe
    # the two layout-relevant X11 facts instead, and publish only if either
    # they or the effective input mode changes.
    DISPLAY="${DISPLAY_ID:-${DISPLAY:-:24}}" xdpyinfo 2>/dev/null |
        awk '
            /^[[:space:]]*dimensions:[[:space:]]*[0-9]+x[0-9]+[[:space:]]+pixels/ { dimensions = $2 }
            /^[[:space:]]*depth of root window:[[:space:]]*[0-9]+[[:space:]]+planes/ { depth = $5 }
            END {
                if (dimensions != "" && depth != "") {
                    print dimensions "/" depth
                    exit 0
                }
                exit 1
            }
        '
}

remember_published_display_state()
{
    PUBLISHED_DISPLAY_SIGNATURE="$1"
    PUBLISHED_INPUT_MODE="$EFFECTIVE_INPUT_MODE"
}

native_touch_available()
{
    local names
    command -v xinput >/dev/null 2>&1 || return 1
    names=$(DISPLAY="${DISPLAY_ID:-${DISPLAY:-:24}}" xinput list --name-only 2>/dev/null || true)
    case "$names" in
        *"${MSYS_CH347_TOUCH_DEVICE:-CH347 XPT2046 Touchscreen}"*) return 0 ;;
        *) return 1 ;;
    esac
}

xtest_available()
{
    local extensions
    extensions=$(DISPLAY="${DISPLAY_ID:-${DISPLAY:-:24}}" xdpyinfo -queryExtensions 2>/dev/null || true)
    case "$extensions" in
        *XTEST*) return 0 ;;
        *) return 1 ;;
    esac
}

set_touch_mouse_mode()
{
    local tmp="$TOUCH_MODE_FILE.$$.tmp"
    mkdir -p "$(dirname -- "$TOUCH_MODE_FILE")"
    printf '%s\n' mouse > "$tmp"
    mv -f "$tmp" "$TOUCH_MODE_FILE"
}

observe_touch_mode()
{
    local mode=touch
    if [ "${CH347_TOUCH:-0}" != "1" ]; then
        EFFECTIVE_INPUT_MODE=none
        return
    fi
    if [ -f "$TOUCH_MODE_FILE" ]; then
        read -r mode < "$TOUCH_MODE_FILE" || mode=touch
    elif [ -n "${CH347_TOUCH_MODE:-}" ]; then
        mode="$CH347_TOUCH_MODE"
    fi
    mode="${mode,,}"
    if [ "$mode" = "mouse" ]; then
        EFFECTIVE_INPUT_MODE=ch347-xtest
    else
        EFFECTIVE_INPUT_MODE=ch347-direct
    fi
}

configure_touch_fallback()
{
    local probes="${MSYS_CH347_NATIVE_TOUCH_PROBES:-20}"
    local count=0

    observe_touch_mode
    if [ "$EFFECTIVE_INPUT_MODE" != "ch347-direct" ] ||
            [ "${MSYS_CH347_XTEST_FALLBACK:-0}" != "1" ]; then
        return 0
    fi
    case "$probes" in
        ''|*[!0-9]*) probes=20 ;;
    esac
    while [ "$count" -lt "$probes" ]; do
        if native_touch_available; then
            echo "msys-ch347-provider: native XInput touch detected"
            return 0
        fi
        count=$((count + 1))
        sleep 0.1
    done
    if ! xtest_available; then
        echo "msys-ch347-provider: native touch missing and XTEST extension unavailable" >&2
        return 1
    fi
    set_touch_mouse_mode
    EFFECTIVE_INPUT_MODE=ch347-xtest
    echo "msys-ch347-provider: native touch missing; enabled optional XTest fallback"
}

claim_ownership()
{
    local tmp="$OWNER_FILE.$$.tmp"
    mkdir -p "$RUN_DIR"
    printf '%s\n' "$OWNER_TOKEN" > "$tmp"
    mv -f "$tmp" "$OWNER_FILE"
}

owns_stack()
{
    [ -f "$OWNER_FILE" ] && [ "$(cat "$OWNER_FILE" 2>/dev/null || true)" = "$OWNER_TOKEN" ]
}

superseded()
{
    if owns_stack; then
        return 1
    fi
    echo "msys-ch347-provider: ownership moved; provider superseded"
    return 0
}

stop_stack()
{
    (
        set +e
        if [ -x "$STOP_SCRIPT" ]; then
            "$STOP_SCRIPT"
        elif [ -f "$STOP_SCRIPT" ]; then
            bash "$STOP_SCRIPT"
        fi
    )
}

cleanup()
{
    if [ "$CLEANED" = "1" ]; then
        return
    fi
    CLEANED=1

    # A previous msysd generation may finish after its replacement has
    # already started.  Only the generation that currently owns the stack is
    # allowed to run the global stop script; otherwise the old EXIT trap can
    # tear down the replacement's X server.
    if owns_stack; then
        stop_stack
        if owns_session_state; then
            rm -f "$SESSION_STATE_FILE" "$SESSION_OWNER_FILE"
        fi
        rm -f "$READY_FILE" "$OWNER_FILE"
    else
        echo "msys-ch347-provider: ownership moved; leaving replacement stack running"
    fi
}

handle_term()
{
    cleanup
    exit 0
}

trap handle_term INT TERM
trap cleanup EXIT

# Claim before inspecting/stopping an existing stack.  This also prevents an
# old provider's delayed EXIT trap from stopping the stack we are about to
# replace.
claim_ownership
# READY_FILE is shared by all generations.  A process must own the stack
# before removing its predecessor's readiness edge, and must never use this
# globally replaceable file as proof that its own publish call succeeded.
rm -f "$READY_FILE"

if [ -f "$PID_FILE" ]; then
    echo "msys-ch347-provider: stale or existing pid file found; stopping previous stack"
    stop_stack
    sleep 1
fi

prepare_mutable_config
migrate_legacy_idle_capture_default
load_touch_calibration

if [ -x "$START_SCRIPT" ]; then
    if ! "$START_SCRIPT"; then
        if superseded; then exit 0; fi
        echo "msys-ch347-provider: start script failed" >&2
        exit 1
    fi
elif ! bash "$START_SCRIPT"; then
    if superseded; then exit 0; fi
    echo "msys-ch347-provider: start script failed" >&2
    exit 1
fi

echo "msys-ch347-provider: started"
echo "msys-ch347-provider: pid-file=$PID_FILE"
echo "msys-ch347-provider: log=$LOG_FILE"

for _ in $(seq 1 80); do
    if signature=$(display_signature); then
        if ! configure_touch_fallback; then
            if superseded; then exit 0; fi
            echo "msys-ch347-provider: touch setup failed" >&2
            exit 1
        fi
        if ! publish_display_state; then
            if superseded; then exit 0; fi
            echo "msys-ch347-provider: display-session publish failed" >&2
            exit 1
        fi
        remember_published_display_state "$signature"
        DISPLAY_READY=1
        if superseded; then exit 0; fi
        echo "msys-ch347-provider: ready-file=$READY_FILE"
        break
    fi
    sleep 0.1
done

if [ "$DISPLAY_READY" != "1" ]; then
    if superseded; then exit 0; fi
    echo "msys-ch347-provider: display did not become ready"
    exit 1
fi

missing_pid_checks=0
display_fail_checks=0
while :; do
    # Replacement claims the stack owner before it stops/restarts the shared
    # X11 pipeline.  An older generation must leave promptly and successfully:
    # otherwise it can report a false failure and, more importantly, its
    # periodic refresh could reclaim display-session.json with stale
    # generation metadata after the replacement has become ready.
    if superseded; then exit 0; fi

    if [ ! -f "$PID_FILE" ]; then
        missing_pid_checks=$((missing_pid_checks + 1))
        if [ "$missing_pid_checks" -ge 3 ]; then
            if superseded; then exit 0; fi
            echo "msys-ch347-provider: pid file missing; provider stopped"
            exit 1
        fi
    else
        missing_pid_checks=0
    fi

    alive=0
    if [ -f "$PID_FILE" ]; then
        while read -r pid; do
            if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
                alive=1
                break
            fi
        done < "$PID_FILE"
    fi

    if [ "$missing_pid_checks" = "0" ] && [ "$alive" = "0" ]; then
        if superseded; then exit 0; fi
        echo "msys-ch347-provider: no live child from pid file"
        tail -80 "$LOG_FILE" 2>/dev/null || true
        exit 1
    fi

    if signature=$(display_signature); then
        display_fail_checks=0
        observe_touch_mode
        if [ "$signature" != "$PUBLISHED_DISPLAY_SIGNATURE" ] ||
                [ "$EFFECTIVE_INPUT_MODE" != "$PUBLISHED_INPUT_MODE" ]; then
            if superseded; then exit 0; fi
            if ! publish_display_state >/dev/null; then
                if superseded; then exit 0; fi
                echo "msys-ch347-provider: display-session state update failed" >&2
                exit 1
            fi
            if superseded; then exit 0; fi
            remember_published_display_state "$signature"
        fi
    else
        display_fail_checks=$((display_fail_checks + 1))
        if [ "$display_fail_checks" -ge 5 ]; then
            if superseded; then exit 0; fi
            echo "msys-ch347-provider: X11 display unavailable"
            exit 1
        fi
    fi

    sleep 2
done
