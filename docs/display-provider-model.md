# Display Provider Model

Files under `manifests/` are compatibility packaging templates. Their
`@package` paths target the archive/package root after a packager promotes the
selected file. Tiny `manifests/scripts/` forwarding entries also keep an
explicit nested source-tree launch valid for development. Release launchers
still use the canonical root manifest from `msys-openstick-ch347` (or another
installed display driver package). This keeps production execution under
`/opt/msys/current` without embedding a mutable `/opt/msys-dev` path.

MSYS should not split SPI and HDMI into two incompatible compositor worlds.

The MVP uses role providers:

- `display-output` for the active output stack.
- `window-policy` for window layout behavior.
- `system-chrome` for bars, navigation, notification shade, and mobile chrome.

For SPI, the existing X11 dummy server and XDamage capture path is a display
provider. For HDMI, Xorg modesetting is another provider. A profile decides which
provider holds the exclusive role.

The X11 root window is the authoritative live pixel geometry. A display provider
that changes mode or rotation must update that root size and its matching input
transform. The native MSYS window policy listens for root `ConfigureNotify` and
reflows applications, bars, dialogs, and shields without restarting the session.
It does not invoke Openbox or depend on a desktop settings daemon.

Logical orientation and physical rotation are deliberately separate. A WM
`set_layout({orientation:"landscape"})` only chooses bar placement and window
policy. A provider that advertises writable physical rotation must also change
the root geometry and publish the matching normalized input matrix. The CH347
reference path does this by capturing a 480x320 logical root, rotating RGB565
into the fixed 320x480 ST7796 panel, and applying the inverse transform to touch
coordinates. HAL commits the provider-owned setting and signals the active
provider generation. The provider changes the existing Xorg root with RandR,
reloads capture and touch mapping in place, then atomically replaces the
same-generation display-session document. Xorg, Shell and application clients
keep their existing `:24` connections; only a real display-provider switch uses
the broader display migration transaction.

Every reference provider publishes the atomic
[`msys.display-session.v1`](display-session-v1.md) document only after a real
X11 probe. HDMI therefore uses `x11-display` readiness rather than treating a
successful `exec(Xorg)` as a ready display. The document records the effective
DISPLAY, live root geometry, and provider-owned input transform.
The canonical active document is `$MSYS_RUNTIME_DIR/display-session.json`;
provider-private ready files remain separate so role switching is atomic.

OpenStick CH347 development profile:

```sh
CH347_TOUCH=1 DEBUG=1 FPS=60 /root/x11display/scripts/start_ch347_dirty_usb_x11.sh
```

Applications started by MSYS inherit the selected visual session and need no
board-specific display argument:

```sh
/opt/example/app
```

The broker resolves the exclusive `display-output` role and injects that
provider's concrete DISPLAY into `windowing.display: "inherit"` components.

Longer term, a Wayland/wlroots compositor can become another provider without
changing launcher, apps, install/update, or role routing.

See [X11 Layout Contract v1](layout-contract.md) for profile, orientation,
insets, runtime switching, and compatibility rules.
