#!/bin/sh
set -eu

for command in Xvfb xdpyinfo xmessage xwininfo; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "test_x11_runtime: missing $command" >&2
        exit 77
    fi
done

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/bin/msys-x11-policy"
DISPLAY_NUMBER=${MSYS_TEST_DISPLAY_NUMBER:-97}
export DISPLAY=":$DISPLAY_NUMBER"
TMP=$(mktemp -d)
export MSYS_RUNTIME_DIR="$TMP/runtime"
mkdir -p "$MSYS_RUNTIME_DIR"
xvfb_pid=
policy_pid=
fixture_pid=
overview_pid=
launcher_guard_pid=

cleanup() {
    if [ -n "$policy_pid" ]; then
        kill "$policy_pid" 2>/dev/null || true
    fi
    if [ -n "$fixture_pid" ]; then
        kill "$fixture_pid" 2>/dev/null || true
    fi
    if [ -n "$overview_pid" ]; then
        kill "$overview_pid" 2>/dev/null || true
    fi
    if [ -n "$launcher_guard_pid" ]; then
        kill "$launcher_guard_pid" 2>/dev/null || true
    fi
    if [ -n "$xvfb_pid" ]; then
        kill "$xvfb_pid" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

Xvfb "$DISPLAY" -screen 0 800x800x24 -ac -nolisten tcp >"$TMP/xvfb.log" 2>&1 &
xvfb_pid=$!
i=0
until xdpyinfo >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 50 ]; then
        cat "$TMP/xvfb.log" >&2
        exit 1
    fi
    sleep 0.05
done

MSYS_LAYOUT_PROFILE=mobile \
MSYS_ORIENTATION=auto \
MSYS_INSETS=auto \
MSYS_DEBUG_LAYOUT=1 \
    "$BIN" >"$TMP/policy.log" 2>&1 &
policy_pid=$!

wait_layout() {
    expected=$1
    i=0
    while [ "$i" -lt 50 ]; do
        layout=$($BIN --print-layout 2>/dev/null || true)
        case "$layout" in
            *"$expected"*)
                printf '%s\n' "$layout"
                return 0
                ;;
        esac
        i=$((i + 1))
        sleep 0.05
    done
    cat "$TMP/policy.log" >&2
    return 1
}

portrait=$(wait_layout "profile=mobile;orientation_policy=auto;insets_policy=auto;orientation=portrait;screen=800,800")

$BIN --set-layout desktop auto 10,20,30,40
desktop=$(wait_layout "profile=desktop;orientation_policy=auto;insets_policy=10,20,30,40")
case "$desktop" in
    *"workarea=40,10,740,760;navigation=bottom;navigation_region=40,770,740,30"*) ;;
    *) echo "unexpected desktop layout: $desktop" >&2; exit 1 ;;
esac

xmessage -title MSYS-Window-Fixture -timeout 30 fixture >"$TMP/fixture.log" 2>&1 &
fixture_pid=$!
window_id=
i=0
while [ "$i" -lt 50 ]; do
    windows=$($BIN --list-windows 2>/dev/null || true)
    window_id=$(printf '%s\n' "$windows" | sed -n \
        's/.*"id":"\([^"]*\)","window_id":"[^"]*","native_id":[^}]*"title":"MSYS-Window-Fixture".*/\1/p')
    if [ -n "$window_id" ]; then
        break
    fi
    i=$((i + 1))
    sleep 0.05
done
case "$window_id" in
    msys.x11-window.v1:*:0x*) ;;
    *) echo "stable window id was not published: $windows" >&2; exit 1 ;;
esac
thumbnail=
i=0
while [ "$i" -lt 50 ]; do
    windows=$($BIN --list-windows 2>/dev/null || true)
    thumbnail=$(printf '%s\n' "$windows" | sed -n \
        's/.*"thumbnail":"\([^"]*\)".*/\1/p')
    case "$windows" in
        *'"title":"MSYS-Window-Fixture"'*'"state":"visible"'*)
            if [ -n "$thumbnail" ] && [ -f "$thumbnail" ]; then
                break
            fi
            ;;
    esac
    i=$((i + 1))
    sleep 0.05
done
if [ -z "$thumbnail" ] || [ ! -f "$thumbnail" ]; then
    echo "window thumbnail was not published: $windows" >&2
    exit 1
fi
magic=$(dd if="$thumbnail" bs=1 count=2 2>/dev/null)
if [ "$magic" != "P6" ]; then
    echo "window thumbnail is not a P6 image: $thumbnail" >&2
    exit 1
fi

# An ordinary launcher is not a compositor.  Once it covers an application,
# XGetImage on the lower window commonly returns an all-black image.  Preserve
# that task's last visible snapshot while still allowing the top launcher to
# get its own preview.
printf 'P6\n1 1\n255\nRGB' >"$thumbnail"
cp "$thumbnail" "$TMP/thumbnail-before-launcher.ppm"
xmessage -title 'MSYS Launcher - Thumbnail Guard' -timeout 30 launcher \
    >"$TMP/launcher.log" 2>&1 &
launcher_guard_pid=$!
i=0
while [ "$i" -lt 50 ]; do
    if xwininfo -name 'MSYS Launcher - Thumbnail Guard' 2>/dev/null |
            grep -q 'Map State: IsViewable'; then
        break
    fi
    i=$((i + 1))
    sleep 0.05
done
windows=$($BIN --list-windows 2>/dev/null || true)
case "$windows" in
    *'"title":"MSYS Launcher - Thumbnail Guard"'*'"kind":"launcher"'*'"state":"visible"'*) ;;
    *) echo "visible launcher guard was not observed: $windows" >&2; exit 1 ;;
esac
if ! cmp -s "$TMP/thumbnail-before-launcher.ppm" "$thumbnail"; then
    echo "application thumbnail was overwritten behind launcher" >&2
    exit 1
fi
kill "$launcher_guard_pid" 2>/dev/null || true
wait "$launcher_guard_pid" 2>/dev/null || true
launcher_guard_pid=

# The task switcher obtains the clean snapshot above before it maps.  Preserve
# a distinctive valid cached image while the overview covers the application;
# a buggy refresh would atomically replace these bytes with a new capture.
printf 'P6\n1 1\n255\nRGB' >"$thumbnail"
cp "$thumbnail" "$TMP/thumbnail-before-overview.ppm"
xmessage -title 'MSYS Recents - Thumbnail Guard' -timeout 30 overview \
    >"$TMP/overview.log" 2>&1 &
overview_pid=$!
i=0
while [ "$i" -lt 50 ]; do
    if xwininfo -name 'MSYS Recents - Thumbnail Guard' 2>/dev/null |
            grep -q 'Map State: IsViewable'; then
        break
    fi
    i=$((i + 1))
    sleep 0.05
done
windows=$($BIN --list-windows 2>/dev/null || true)
case "$windows" in
    *'"title":"MSYS Recents - Thumbnail Guard"'*'"role":"task-switcher"'*'"state":"visible"'*) ;;
    *) echo "visible task switcher was not observed: $windows" >&2; exit 1 ;;
esac
if ! cmp -s "$TMP/thumbnail-before-overview.ppm" "$thumbnail"; then
    echo "application thumbnail was overwritten behind task switcher" >&2
    exit 1
fi
case "$windows" in
    *'"thumbnail":"'*) ;;
    *) echo "valid cached thumbnail was not reused: $windows" >&2; exit 1 ;;
esac

# A missing cache remains missing until the overview is gone.  Capturing here
# would publish an obscured first frame.
rm -f "$thumbnail"
windows=$($BIN --list-windows 2>/dev/null || true)
if [ -e "$thumbnail" ]; then
    echo "missing thumbnail was captured behind task switcher" >&2
    exit 1
fi
case "$windows" in
    *'"thumbnail":"'*)
        echo "missing thumbnail was published behind task switcher: $windows" >&2
        exit 1
        ;;
esac

kill "$overview_pid" 2>/dev/null || true
wait "$overview_pid" 2>/dev/null || true
overview_pid=
i=0
while [ "$i" -lt 50 ]; do
    windows=$($BIN --list-windows 2>/dev/null || true)
    if [ -f "$thumbnail" ]; then
        break
    fi
    i=$((i + 1))
    sleep 0.05
done
if [ ! -f "$thumbnail" ]; then
    echo "thumbnail capture did not resume after task switcher closed: $windows" >&2
    exit 1
fi

wait_window_text() {
    expected=$1
    i=0
    while [ "$i" -lt 50 ]; do
        windows=$($BIN --list-windows 2>/dev/null || true)
        case "$windows" in
            *"$expected"*) return 0 ;;
        esac
        i=$((i + 1))
        sleep 0.05
    done
    echo "window state was not observed: $expected" >&2
    echo "$windows" >&2
    return 1
}

$BIN --window-minimize "$window_id"
wait_window_text '"title":"MSYS-Window-Fixture","identity":"Xmessage","component":null,"role":"application","kind":"application","state":"minimized"'
$BIN --window-focus "$window_id"
wait_window_text '"title":"MSYS-Window-Fixture","identity":"Xmessage","component":null,"role":"application","kind":"application","state":"visible"'
$BIN --window-move-resize "$window_id" 100 120 320 240
wait_window_text '"geometry":{"x":100,"y":120,"width":320,"height":240}'
$BIN --window-close "$window_id"
i=0
while kill -0 "$fixture_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    i=$((i + 1))
    sleep 0.05
done
if kill -0 "$fixture_pid" 2>/dev/null; then
    echo "window close did not reach WM_DELETE_WINDOW" >&2
    exit 1
fi
fixture_pid=
i=0
while [ -e "$thumbnail" ] && [ "$i" -lt 50 ]; do
    i=$((i + 1))
    sleep 0.05
done
if [ -e "$thumbnail" ]; then
    echo "window thumbnail survived window destruction: $thumbnail" >&2
    exit 1
fi
if $BIN --window-focus "$window_id" 2>/dev/null; then
    echo "stale stable window id remained actionable" >&2
    exit 1
fi

$BIN --sync-display-session 800 800 24 1 1,0,0,0,1,0,0,0,1
wait_layout "screen=800,800" >/dev/null
if $BIN --sync-display-session 800 800 24 1 1,0,0,0,0,0,0,0,1 2>/dev/null; then
    echo "singular display-session transform was accepted" >&2
    exit 1
fi

$BIN --debug-root-size 800 480
landscape=$(wait_layout "orientation=landscape;screen=800,480")
case "$landscape" in
    *"workarea=40,10,740,440;navigation=bottom;navigation_region=40,450,740,30"*) ;;
    *) echo "unexpected resized layout: $landscape" >&2; exit 1 ;;
esac

printf '%s\n%s\n%s\n' "$portrait" "$desktop" "$landscape"
echo "test_x11_runtime: ok"
