# msys-x11-session

X11 session integration for MSYS.

MVP paths:

- SPI display: Xorg dummy + WM + existing `x11display` capture/sink pipeline.
- HDMI display: Xorg modesetting can be represented by a different provider.
- HDMI + SPI mirror: HDMI is primary; XDamage capture mirrors to SPI sink.

This repository only describes and wraps X11 providers. It does not replace the
existing `x11display` repository or its Git history.

The canonical installable system package is [`manifest.json`](manifest.json),
with package id `org.msys.x11.session`. Its window policy is
`org.msys.x11.session:window-policy`; it is not owned by the shell package.
The primary `window-policy` component is one C process. The same
`@package/bin/msys-x11-policy` ELF owns the X11 redirect, consumes the SDK C
mIPC wire, publishes readiness, and handles window-manager calls. It does not
fork a policy helper and it does not start Openbox. Small bounded worker
threads keep Home/Back/Apps calls reentrant while Core calls back into
`activate_component` or `recents`; they do not add resident processes.

`window-policy-python` remains an on-demand, 30-second idle compatibility
fallback for the legacy `window-policy` role at lower priority. It does not
claim the strict `window-manager.v1` role, whose contract correctly requires a
resident background/session provider. It is not in profile startup and
therefore does not consume memory when the native provider is healthy. Its
package-local entry point still runs without an external `PYTHONPATH`.

The native policy owns mobile, kiosk, and desktop placement directly through
Xlib; Openbox is neither started nor required. It uses live X11 root geometry,
stable application identity, and the versioned profile/orientation/insets
contract documented in [docs/layout-contract.md](docs/layout-contract.md).
The toolkit-neutral [window-manager v1 contract](docs/window-manager-v1.md)
adds generation-checked stable window ids plus focus, minimize, move, resize,
move-resize, and graceful close operations. It also defines one typed
navigation entrypoint for Back, Home, Apps, and committed pill gestures.
Home delegates to `msys.core.activate_role({"role":"launcher"})`; the selected
provider's manifest identity and title are returned through
`window-manager.activate_component`, so this package contains no launcher
package-id or title convention. Home runs on a worker to keep the private mIPC
reader available for that reentrant activation callback.
The CH347 and HDMI providers publish the live
[`msys.display-session.v1`](docs/display-session-v1.md) ready/state contract,
including the actual DISPLAY, X11 root geometry, and effective input transform.
Their manifest roles claim the exact stable descriptors
`org.msys.role.window-manager.v1@1.0.0` and
`org.msys.role.display-output.v1@1.0.0`; compatibility manifests carry the
same claims.
The native development helper also provides identity/title-addressed XTest
swipe/drag injection, with no xdotool runtime dependency.

The native provider implements `get_display_session`, `get_layout`,
`set_layout`, `list_windows`, `recents`, `activate_component`, Home, Back,
Apps, `close_active`, and generation-checked typed window actions. Native
`list_windows`/`recents` currently expose the real X11 role stack; a supervised
component which is ready but has not mapped its first surface is only added by
the Python compatibility provider. Native display-session publication covers
the output-only and CH347 direct/XTest modes used by the reference providers.
The optional `xinput` property-probe mode intentionally falls back to the
Python publisher rather than adding a subprocess parser to the resident C
agent.

Replaceable virtual keyboards publish `MSYS_WINDOW_ROLE=input-method` (the
`role:` prefix and `virtual-keyboard`, `on-screen-keyboard`, or
`soft-keyboard` aliases are accepted). The stock `org.msys.input.touch`
identity is recognized directly; vendors that cannot publish a role can use
the comma-separated `MSYS_X11_INPUT_METHOD_IDENTITIES` compatibility setting.
Only a window classified this way may enter policy as `override_redirect`, so
ordinary toolkit menus and tooltips remain unmanaged.

Native compilation is an explicit development/package step. No provider or
agent invokes `make`, `cc`, or a target package manager at runtime:

```sh
make all                 # build bin/msys-x11-policy
make test                # native and zero-external-PYTHONPATH Python tests
make package             # dist/org.msys.x11.session-0.2.4.tar.gz
make package-test        # extract and import-test the installed-root layout
```

On the constrained target, compilation and memory probing are deliberately
single-job and isolated from the live `:24` display:

```sh
sh scripts/build_aarch64_j1.sh
sh scripts/probe_native_rss.sh :98
```

Profiles using this package select both policy roles from the same component:

```json
{
  "window-policy": ["org.msys.x11.session:window-policy"],
  "window-manager": ["org.msys.x11.session:window-policy"]
}
```

The default HDMI candidate is `org.msys.x11.session:hdmi-output`. Existing
OpenStick profiles can continue selecting
`org.msys.openstick.ch347:x11-spi-touch-output` unchanged.

Local verification:

```sh
make test
env -u PYTHONPATH python3 -m unittest discover -s tests -v
```
