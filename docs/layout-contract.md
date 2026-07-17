# X11 Layout Contract v1

MSYS separates physical output configuration from window layout:

- The active `display-output` provider owns Xorg, modes, framebuffer rotation,
  mirroring, and input calibration.
- The `window-policy` provider reads the X11 root-window size and places client
  windows inside that real coordinate space.
- A profile selects layout policy using three environment values. No program
  assumes a 320x480 display.

This boundary requires no systemd, D-Bus, logind, Openbox, or host package
manager.

## Startup contract

The `window-policy` component receives:

| Variable | Values | Meaning |
| --- | --- | --- |
| `MSYS_LAYOUT_PROFILE` | `mobile`, `kiosk`, `desktop` | Window placement strategy |
| `MSYS_ORIENTATION` | `auto`, `portrait`, `landscape` | Navigation/layout orientation policy |
| `MSYS_INSETS` | `auto` or `top,right,bottom,left` | Reserved root-window edges in pixels |

The recommended adaptive mobile profile is:

```json
"env": {
  "MSYS_LAYOUT_PROFILE": "mobile",
  "MSYS_ORIENTATION": "auto",
  "MSYS_INSETS": "auto"
}
```

Visual components declare `windowing.display: "inherit"`; `msysd` injects the
DISPLAY exported by the profile-selected `display-output` provider. Layout
profiles therefore never hard-code an SPI or HDMI display address.

Insets are ordered clockwise starting at the top. Negative values, missing
fields, extra fields, unknown profiles, and unknown orientations are rejected.
Explicit insets larger than the root window are clamped so the work area always
retains at least one pixel.

For an explicit landscape mobile layout, a non-zero right inset with no bottom
inset selects right-edge navigation; a non-zero bottom inset with no right inset
keeps bottom navigation. This makes the safe-area declaration and UI edge agree
and preserves legacy top/bottom configurations.

`auto` orientation follows the live X11 root aspect ratio. Explicit portrait or
landscape is useful for a product that fixes its physical mounting regardless
of the current pixel aspect ratio.

## Strategies

| Profile | Automatic insets | Application placement | Overlays |
| --- | --- | --- | --- |
| Mobile portrait | adaptive top and bottom bars | fills work area | navigation at bottom |
| Mobile landscape | adaptive top and right bars | fills work area | navigation at right |
| Kiosk | zero | fills the complete root window | bars overlay if the profile starts them |
| Desktop | adaptive top and bottom panels | preserves requested geometry, bounded to work area | dialogs are centered; notifications use the top-right |

Adaptive metrics derive from the current short side and are bounded to useful
touch sizes. The bounds are UI metrics, not assumptions about display
resolution.

An `input-method` surface is a floating overlay in every profile. Its requested
geometry is preserved whenever valid and bounded only to the effective work
area; mobile and kiosk placement never stretch it through the ordinary
application full-screen rule. Its layer is above applications and the launcher
but below recents, chooser, notifications, chrome, navigation, transitions,
and the screen shield.

A real kiosk profile should omit `system-chrome`, `navigation-bar`, notification
center, and task-switcher startup providers if the product must expose only one
application. If one of those providers is started intentionally, it is treated
as an overlay and does not reduce the kiosk application rectangle.

## Runtime and X11 properties

The policy stores the requested configuration atomically on the root window:

```text
_MSYS_LAYOUT_CONFIG_V1 =
msys.layout.v1;profile=mobile;orientation=auto;insets=auto
```

It publishes the resolved state separately:

```text
_MSYS_LAYOUT_EFFECTIVE_V1 =
msys.layout.effective.v1;profile=mobile;orientation_policy=auto;
insets_policy=auto;orientation=landscape;screen=800,480;
insets=60,60,0,0;workarea=0,60,740,420;navigation=right;
navigation_region=740,60,60,420
```

The line breaks above are for readability; the X11 property is one line.
`navigation_region` is the exact pixel rectangle assigned to the replaceable
navigation provider, including overlay profiles whose inset is zero. System UI
providers can read the effective property or call `get_layout` and
must render according to their actual window aspect. In particular, a mobile
navigation provider must stack controls vertically when `navigation=right`.

Native diagnostics and runtime switching:

```sh
DISPLAY=:24 bin/msys-x11-policy --print-layout
DISPLAY=:24 bin/msys-x11-policy --set-layout mobile landscape auto
DISPLAY=:24 bin/msys-x11-policy --set-layout kiosk auto 0,0,0,0
DISPLAY=:24 bin/msys-x11-policy --set-layout desktop auto auto
```

Development tools can inject a real pointer gesture without xdotool. Coordinates
are target-window-local non-negative pixels, duration is a strict `1..10000`
milliseconds, and endpoints must remain inside both the visible target and X11
root:

```sh
DISPLAY=:24 bin/msys-x11-policy \
  --debug-swipe-identity org.example.app 20 300 20 40 240
DISPLAY=:24 bin/msys-x11-policy \
  --debug-swipe-window org.example.app "Example" 20 300 20 40 240
```

The helper dynamically verifies XTEST, moves to the start point, presses
button 1, emits roughly 16 ms interpolated motion frames, and always releases.
`--debug-swipe-title` is available for identity-less legacy windows;
`--debug-drag-{identity,title,window}` hold button 1 for 650 milliseconds
before the first motion so long-press interactions can be tested, while the
corresponding `--debug-swipe-*` forms begin moving immediately.

Equivalent window-policy mIPC calls:

```text
get_layout({})
get_display_session({})
set_layout({"profile":"mobile","orientation":"landscape","insets":"auto"})
set_layout({"profile":"desktop","insets":{"top":32,"right":0,"bottom":40,"left":0}})
```

`set_layout(mode)` remains accepted through the `mode` payload alias, but new
callers should send `profile` explicitly. A single root property carries the
whole request, so observers never see a half-applied profile/orientation/insets
combination.

`get_layout` also merges the active `msys.display-session.v1` document under
`display_session`, reports `display_consistent`, and projects the navigation
rectangle through the inverse provider input matrix as
`navigation_input_region`. This gives HAL and diagnostic clients a typed view
of physical geometry, effective logical layout, and the touch-space hit region
without guessing `:24` or reading board-specific files.

After `set_layout` has observed the matching native effective property, the
agent publishes `msys.layout.changed`. Its `msys.layout.changed.v1` payload
contains the profile, requested/effective orientation, effective geometry,
insets/work area/navigation region, and active display provider generation.
Rejected or unobserved layout changes never emit the event.

## Dynamic resolution and rotation

The policy subscribes to root `ConfigureNotify`. Whenever Xorg changes root
width or height it:

1. resolves effective orientation and automatic metrics from the new size;
2. updates `_MSYS_LAYOUT_EFFECTIVE_V1`;
3. reflows all mapped managed windows;
4. restores deterministic overlay layers.

The policy deliberately does not invoke `xrandr` itself. Rotating an HDMI mode,
changing the SPI dummy framebuffer, or transforming touch coordinates belongs
to the selected `display-output` provider. Once that provider updates the X11
root dimensions, layout follows without a restart.

The package-local agent watches
`${MSYS_DISPLAY_SESSION_STATE_FILE:-$MSYS_RUNTIME_DIR/display-session.json}`.
Each new geometry/input-transform generation is validated and projected into
`_MSYS_DISPLAY_SESSION_LAYOUT_V1`; the native policy accepts it only when its
geometry matches the live root and then performs a full reflow. A state that
moves to another `DISPLAY` causes the policy component to restart so the broker
can inject the newly selected display-output provider's address. A bounded
one-second grace (configurable with `MSYS_DISPLAY_SWITCH_GRACE`) lets the
exclusive-role lease commit before that restart and avoids binding once more
to the outgoing provider during a live switch.

## Compatibility

When `MSYS_INSETS` is absent, the native policy translates legacy
`MSYS_CHROME_TOP` and `MSYS_CHROME_BOTTOM` into explicit
`top,0,bottom,0` insets. `MSYS_WINDOW_POLICY=mobile|kiosk|desktop` is accepted
only when `MSYS_LAYOUT_PROFILE` is absent. Canonical variables always win.

The legacy path preserves existing deployments, but explicit top/bottom values
cannot automatically move navigation to the right after a landscape resize.
Profiles that need live rotation should migrate to all three canonical values
and normally use `MSYS_ORIENTATION=auto`, `MSYS_INSETS=auto`.

## Validation

Pure layout rules have no X11 dependency and are exercised with:

```sh
make test
```

Python contract and window-policy tests run with:

```sh
env -u PYTHONPATH python3 -m unittest discover -s tests -v
```

An Xvfb integration check can change `_MSYS_LAYOUT_CONFIG_V1` and the root size,
then assert that `--print-layout` reports the new profile, dimensions, insets,
and work area. This is the same ConfigureNotify path used by HDMI and SPI Xorg.

```sh
make integration-test
```

Xvfb exposes a fixed mode, so this test enables `MSYS_DEBUG_LAYOUT=1` and injects
a synthetic root `ConfigureNotify` through `--debug-root-size`. Production mode
changes arrive from Xorg with `send_event=false`; synthetic resize events are
ignored unless that explicit test flag is enabled.
