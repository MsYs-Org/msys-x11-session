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

## Application transition lifecycle

The native provider exposes a lightweight observation contract for phone-style
launch and close animations. It does not render an overlay and does not start,
stop, restart, capture, or composite an application. The Shell paints its own
icon/color layer immediately, while the window policy reports when the exact
component surface actually becomes viewable or disappears.

Callers should generate the transition id before painting so an event can be
matched even if it arrives before the RPC return:

```text
begin_launch_transition({
  transition_id: "shell-42",
  component: "org.example.viewer:main",
  identity: "org.example.viewer",       # optional presentation metadata
  icon: "/package/icons/viewer.png",    # optional, not opened by policy
  color: "#f4f7ff",                    # optional #RRGGBB or #AARRGGBB
  origin: {x: 24, y: 72, width: 64, height: 64}, # optional
  timeout_ms: 8000                      # optional, 100..30000
}) -> {
  ok: true,
  schema: "msys.window-transition.v1",
  transition_id: "shell-42",
  action: "launch",
  component: "org.example.viewer:main",
  state: "waiting-surface",
  timeout_ms: 8000
}

begin_close_transition({...same fields...})
begin_transition({...same fields..., action: "launch" | "close"})
cancel_transition({transition_id: "shell-42", reason: "core-start-failed"})
```

Accepted transactions publish `msys.window.transition` with schema
`msys.window-transition.v1`. The stable phases are:

```text
requested
surface-ready  # launch: exact component has an IsViewable application window
surface-gone   # close: a previously observed exact surface is no longer viewable
failed         # reason is surface-timeout or the caller's cancellation reason
```

`surface-ready` includes the generation-checked `window_id` and observed
`window_identity`. Every event repeats `transition_id`, `action`, `component`,
`identity`, `timeout_ms`, and the bounded `visual` hints. A close transaction
must begin while the target surface still exists; it cannot mistake an already
missing window for a successful exit.

The intended launch sequence is: paint the Shell overlay, begin observation,
call Core `start`, then fade/scale the overlay only after `surface-ready`. If
Core rejects the start, call `cancel_transition` and remove the overlay while
showing the returned reason. Closing follows the same sequence with
`begin_close_transition`, an explicit close action, and `surface-gone`.
Timeout and cancellation only end the visual transaction; they never kill or
restart a component.

Observation is event driven. The provider performs one initial lookup and then
rescans only after X11 map, unmap, destroy, or identity-property changes. It
does not add a timer-driven screenshot/render loop and does not touch display
dirty tracking. At most eight transitions may be active, and one component may
have only one active transition.

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
maximize_window({window_id})
restore_window({window_id})
snap_left_window({window_id})
snap_right_window({window_id})
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
Maximize and snap are desktop-only application actions. The provider stores the
pre-placement geometry on the X11 window and clamps it to the current work area
when restoring. Snap modes are rejected by mobile and kiosk profiles. Successful
actions report the actual `geometry`, `placement`, `state`, `profile`, and
optional `restore_geometry`; the same record is published as
`msys.window.action`. A live profile change reflows existing windows without
restarting X11 or its selected display-output provider.

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
