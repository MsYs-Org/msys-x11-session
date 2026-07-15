# MSYS window-manager v1

`org.msys.x11.session:window-policy` provides the replaceable
`window-manager` role and `org.msys.window-manager.v1` interface. The contract
is toolkit-neutral: Qt, Electron, Tk, SDL, and native Xlib clients are all
identified through manifest-injected identity rather than their title.

## Window snapshot and identity

```text
list_windows({}) -> {
  schema: "msys.window-list.v1",
  windows: [msys.window.v1, ...]
}
recents({}) -> the same snapshot in Core foreground order
```

Each mapped or previously mapped top-level client gets an immutable X11
`_MSYS_WINDOW_ID_V1` property. Its opaque public value resembles
`msys.x11-window.v1:<generation>:0x<resource>`. Callers must treat the complete
string as an opaque `window_id`; `native_id` is diagnostic only.

For a mapped application or launcher, the reference X11 provider also writes a
bounded P6 preview under `$MSYS_RUNTIME_DIR/window-thumbnails` and returns its
absolute path in the optional `thumbnail` field. The image is atomically
replaced, never exceeds 288x360 or 2 MiB, and is removed after the X11 window is
destroyed. Settled XDamage events replace an early mapped/background frame once
the application paints real pixels; startup bursts are coalesced without a
polling screenshot timer. Capture only occurs while the application is
unobscured and does not issue any X rendering or display damage. While a
task-switcher window is mapped, the provider reuses an existing preview instead
of capturing pixels covered by that overview. A missing preview stays absent
until the task switcher is hidden. Task switchers must therefore treat it as an
optional cache: a minimized, legacy, or concurrently destroyed window may have
no preview and should fall back to its application icon.

The generation prevents X11 resource reuse from redirecting a stale action to
a different client. Every action verifies that the complete property still
belongs to the same root-level window. The id remains valid while that window
exists, including while minimized and across a policy-process restart. It
becomes permanently stale when the client destroys the window.

`msys.window.v1` contains:

```json
{
  "schema": "msys.window.v1",
  "id": "msys.x11-window.v1:...:0x...",
  "window_id": "msys.x11-window.v1:...:0x...",
  "native_id": "0x...",
  "title": "presentation text",
  "identity": "org.example.viewer",
  "component": "org.example.viewer:main",
  "role": "application",
  "kind": "application",
  "state": "visible",
  "geometry": {"x": 0, "y": 48, "width": 320, "height": 384},
  "managed": true,
  "source": "x11"
}
```

`state` is `visible`, `minimized`, `hidden`, or `pending-surface`. A supervised
component can briefly have `pending-surface` after readiness and before its
first X11 map; that entry deliberately has no invented window id. Core
lifecycle records and X11 surfaces are joined only by exact component or
identity, never by title.

## Typed window actions

All actions return `schema: "msys.window-action.v1"`, `ok`, `action`, and the
requested `window_id`. Methods accept `window_id`; `id` remains a compatibility
alias.

```text
focus_window({window_id})
minimize_window({window_id})
move_window({window_id, x, y})
resize_window({window_id, width, height})
move_resize_window({window_id, x, y, width, height})
close_window({window_id})
window_action({action, window_id, ...})
```

Coordinates are signed 16-bit integers. Dimensions are `1..32767`. Desktop
layout accepts requested geometry and clamps it to the work area. Mobile and
kiosk layout remain authoritative and can resolve the request to their own
full-screen geometry. Focus maps a minimized surface before assigning input
focus. Minimize uses `WM_STATE=IconicState`. Close sends `WM_DELETE_WINDOW`
when supported and falls back to `XKillClient` only for a legacy client that
does not implement the protocol.

For a supervised application, `close_window` and `close_active` stop the owning
component through Core so restart and transition policy stay coherent. Direct
native close is used only for external X11 clients.

Typed failure reasons include `invalid-window-id`, `invalid-geometry`,
`stale-or-missing-window`, `component-stop-failed`, and `x11-action-failed`.

## Navigation and overlay Back

Existing methods remain available:

```text
back({})
close_active({})  # explicit lifecycle close; not an alias of Back
home({})
recents({})       # data snapshot used by the task-switcher provider
```

Buttons and a committed navigation-pill gesture may use one entrypoint:

```text
navigation_action({action: "back" | "home" | "apps", input: "button" | "swipe"})
navigate(...)  # alias
```

`apps` resolves the active `task-switcher` role and calls `show`; it does not
name a shell package. Home resolves the active launcher role. Both operations
run outside the provider's single mIPC reader because their role transaction
calls back into this provider (`activate_component` or `recents`). This avoids
a navigation-button deadlock.

After overlays, Back first asks Core to deliver `navigation_back()` to the
foreground component when its manifest provides
`org.msys.application-navigation.v1`:

```text
navigation_back({}) -> { handled: true, page? }
navigation_back({}) -> { handled: false }  # application root page
```

`handled:true` leaves lifecycle and stacking unchanged. At the root page, the
policy refreshes Core's foreground stack, asks Core to mark that component
`background`, captures its unobscured task preview, minimizes its X11 surface,
and activates the selected launcher. The process remains alive and the task
remains in Recents; Back never restores a different task and never calls
`stop`, `WM_DELETE_WINDOW`, or `XKillClient`. Only an explicit `close_active`
or `close_window` terminates an application. A failed minimize/Home transaction
best-effort starts and focuses the original component again. A declared
navigation provider which times out or returns an invalid reply fails closed.
Overlay-only Back returns immediately after dismissing one layer and never
changes the application destination.

The launcher may remain mapped underneath an active application. Native Back
therefore treats it as Home only when Core reports no active (non-background)
foreground component. When an application is active, policy resolves the X11
surface by its exact `_MSYS_COMPONENT_ID`; stacking order or a mapped launcher
cannot redirect Back away from that application.

Back examines the real top-to-bottom X11 role stack and dismisses exactly one
top-most overlay before touching an application:

1. `screen-shield.hide()`;
2. `chooser.cancel_choice()`;
3. `notification-center.hide()`;
4. `task-switcher.hide()`;
5. `input-method.hide()`.

The ordering above describes layer priority; only the actually top-most
visible role is called. A failed role dismissal fails closed and never closes
the obscured application. The launcher is treated as Home, not as an app to
close.

Window role, component, app id, process environment, and `WM_CLASS` are stable
classification inputs. Historic `MSYS ...` titles are retained only when a
window publishes none of those identities. Such records explicitly report
`compatibility: "legacy-title"`.
