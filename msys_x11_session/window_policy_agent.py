from __future__ import annotations

import json
import os
import signal
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping, Sequence

from .display_session import DisplaySessionError, load_state
from .layout_contract import LayoutConfig, parse_effective
from .mipc import MsysClient


# Titles are retained strictly for clients from before the manifest window
# identity contract.  New policy decisions use the native window role and
# stable identity fields returned by ``--list-windows``.
LEGACY_SYSTEM_TITLES = (
    "MSYS Chrome",
    "MSYS Launcher",
    "MSYS Navigation",
    "MSYS Notifications",
    "MSYS Notification Center",
    "MSYS Recents",
    "MSYS Intent Chooser",
    "MSYS Screen Shield",
    "msys-notification-host",
    "msys-task-switcher-host",
    "msys-intent-chooser-host",
)
LEGACY_DISMISSIBLE_TITLES = (
    "MSYS Screen Shield",
    "MSYS Intent Chooser",
    "MSYS Notification Center",
    "MSYS Recents",
)


@dataclass
class XWindow:
    xid: str
    title: str
    window_id: str = ""
    identity: str = ""
    component: str = ""
    role: str = ""
    kind: str = "application"
    state: str = "visible"
    geometry: dict[str, int] = field(default_factory=dict)
    compatibility_title: bool = False

    def descriptor(self) -> dict[str, Any]:
        """Return the versioned, toolkit-neutral window-manager record."""

        window_id = self.window_id or self.xid
        result: dict[str, Any] = {
            "schema": "msys.window.v1",
            "id": window_id,
            "window_id": window_id,
            "native_id": self.xid,
            "title": self.title,
            "identity": self.identity or None,
            "component": self.component or None,
            "role": self.role or None,
            "kind": self.kind,
            "state": self.state,
            "source": "x11",
        }
        if self.geometry:
            result["geometry"] = dict(self.geometry)
        if self.compatibility_title:
            result["compatibility"] = "legacy-title"
        return result


class DisplaySessionChanged(RuntimeError):
    """The selected display moved and the native X11 connection must restart."""


_active_display_session: dict[str, Any] | None = None
_active_display_session_error: str | None = None


def run(argv: list[str], timeout: float = 2.0) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.setdefault("DISPLAY", env.get("DISPLAY_ID", ":0"))
    return subprocess.run(argv, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)


def parse_xwininfo_tree(text: str) -> list[XWindow]:
    result: list[XWindow] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("0x") or '"' not in stripped:
            continue
        xid = stripped.split(None, 1)[0]
        title = stripped.split('"', 2)[1]
        if title and not title.startswith("has no name"):
            result.append(XWindow(xid=xid, title=title))
    return result


def parse_native_window_list(text: str) -> list[XWindow]:
    """Decode the native policy's bounded JSON window snapshot.

    The native helper reads metadata using Xlib and `/proc`, so callers do not
    need xprop, a toolkit binding, or title conventions.  Unknown fields are
    deliberately ignored to keep the v1 reader forward compatible.
    """

    try:
        document = json.loads(text)
    except (json.JSONDecodeError, TypeError) as exc:
        raise ValueError(f"invalid native window list: {exc}") from exc
    if not isinstance(document, dict) or document.get("schema") != "msys.window-list.v1":
        raise ValueError("invalid native window list schema")
    raw_windows = document.get("windows")
    if not isinstance(raw_windows, list):
        raise ValueError("native window list has no windows array")

    windows: list[XWindow] = []
    for raw in raw_windows:
        if not isinstance(raw, dict):
            continue
        xid = str(raw.get("native_id") or "").strip().lower()
        window_id = str(raw.get("id") or "").strip()
        if not xid.startswith("0x") or not window_id.startswith("msys.x11-window.v1:"):
            continue
        geometry = raw.get("geometry")
        if not isinstance(geometry, dict):
            geometry = {}
        try:
            typed_geometry = {
                name: int(geometry[name])
                for name in ("x", "y", "width", "height")
                if name in geometry
            }
        except (TypeError, ValueError):
            typed_geometry = {}
        windows.append(XWindow(
            xid=xid,
            title=str(raw.get("title") or ""),
            window_id=window_id,
            identity=str(raw.get("identity") or ""),
            component=str(raw.get("component") or ""),
            role=str(raw.get("role") or ""),
            kind=str(raw.get("kind") or "application"),
            state=str(raw.get("state") or "hidden"),
            geometry=typed_geometry,
            compatibility_title=bool(raw.get("compatibility_title", False)),
        ))
    return windows


def list_windows() -> list[XWindow]:
    """Return top-level windows in top-to-bottom stacking order.

    Installed 0.1.6 binaries do not yet expose the native snapshot.  The
    xwininfo bridge remains a read-only compatibility path during an atomic
    package upgrade and is intentionally not used for typed window actions.
    """

    try:
        result = run([native_policy_binary(), "--list-windows"], timeout=3)
        if result.returncode == 0:
            return parse_native_window_list(result.stdout)
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    result = run(["xwininfo", "-root", "-tree"], timeout=3)
    return parse_xwininfo_tree(result.stdout)


def is_visible_window(window: XWindow) -> bool:
    if window.window_id:
        return window.state == "visible"
    try:
        result = run(["xwininfo", "-id", window.xid, "-stats"], timeout=1)
    except Exception:
        return False
    if result.returncode != 0:
        return False
    state = result.stdout.lower().replace(" ", "")
    return "mapstate:isviewable" in state


def is_system_window(window: XWindow) -> bool:
    if window.role and window.role != "application":
        return True
    if window.kind and window.kind not in {"application", "unknown"}:
        return True
    if window.identity or window.component:
        return False
    return any(window.title.startswith(prefix) for prefix in LEGACY_SYSTEM_TITLES)


def user_windows(*, include_minimized: bool = False) -> list[XWindow]:
    windows = []
    for window in list_windows():
        if is_system_window(window):
            continue
        if not is_visible_window(window) and not (
            include_minimized and window.state == "minimized"
        ):
            continue
        windows.append(window)
    return windows


def core_foreground_windows() -> list[dict]:
    """Return MSYS-owned foreground components in broker stacking order.

    The broker owns process lifecycle, so this is the authoritative source for
    recents and Back.  Raw X11 enumeration remains a fallback for applications
    started outside MSYS.
    """
    response = MsysClient.public_call(
        "msys.core",
        "foreground_stack",
        {},
        timeout=3,
    )
    if response.get("type") != "return":
        raise RuntimeError(response.get("message") or response.get("code") or "foreground_stack failed")
    windows = response.get("payload", {}).get("windows", [])
    return [dict(item) for item in windows if isinstance(item, dict) and item.get("component")]


def merge_managed_windows(
    managed: Sequence[Mapping[str, Any]], native: Sequence[XWindow]
) -> list[dict[str, Any]]:
    """Join Core lifecycle records with native surfaces without title matching."""

    available = list(native)
    used: set[str] = set()
    result: list[dict[str, Any]] = []
    for raw in managed:
        item = dict(raw)
        component = str(item.get("component") or "").strip()
        identity = str(item.get("identity") or "").strip().casefold()
        match: XWindow | None = None
        for candidate in available:
            key = candidate.window_id or candidate.xid
            if key in used or is_system_window(candidate):
                continue
            if component and candidate.component == component:
                match = candidate
                break
        if match is None and identity:
            for candidate in available:
                key = candidate.window_id or candidate.xid
                if key in used or is_system_window(candidate):
                    continue
                if candidate.identity.casefold() == identity:
                    match = candidate
                    break
        if match is not None:
            key = match.window_id or match.xid
            used.add(key)
            descriptor = match.descriptor()
            descriptor.update({
                "component": component,
                "managed": True,
            })
            if not descriptor.get("title"):
                descriptor["title"] = str(item.get("title") or component)
            descriptor["component_state"] = item.get("state")
            result.append(descriptor)
        else:
            # A process may become ready just before its first X11 map. Keep it
            # actionable through Core while making the lack of a surface
            # explicit instead of inventing a window id from its title.
            result.append({
                "schema": "msys.window.v1",
                "id": "",
                "window_id": None,
                "native_id": None,
                "component": component,
                "identity": str(item.get("identity") or "") or None,
                "title": str(item.get("title") or component),
                "role": "application",
                "kind": "application",
                "state": "pending-surface",
                "component_state": item.get("state"),
                "source": "msys.core",
                "managed": True,
            })

    for candidate in available:
        key = candidate.window_id or candidate.xid
        if key in used or is_system_window(candidate):
            continue
        if candidate.state not in {"visible", "minimized"}:
            continue
        result.append(candidate.descriptor())
    return result


def active_or_top_user_window() -> XWindow | None:
    # Both the native list and the compatibility tree are top-most first.
    windows = user_windows()
    return windows[0] if windows else None


def top_content_window() -> XWindow | None:
    for window in list_windows():
        if window.role in {
            "system-chrome",
            "navigation-bar",
            "notification-presenter",
            "input-method",
            "transition-presenter",
            "screen-shield",
            "chooser",
            "notification-center",
            "task-switcher",
        }:
            continue
        if not window.role and not window.identity and any(
            window.title.startswith(prefix)
            for prefix in LEGACY_SYSTEM_TITLES
            if prefix not in {"MSYS Launcher"}
        ):
            continue
        if not is_visible_window(window):
            continue
        return window
    return None


def top_dismissible_window() -> XWindow | None:
    for window in list_windows():
        if not is_visible_window(window):
            continue
        if window.role in {
            "screen-shield",
            "chooser",
            "notification-center",
            "task-switcher",
            "input-method",
        }:
            return window
        if (
            not window.role
            and not window.identity
            and any(window.title.startswith(prefix) for prefix in LEGACY_DISMISSIBLE_TITLES)
        ):
            return window
    return None


def close_window(window: XWindow) -> dict:
    if window.window_id:
        return window_action("close", window.window_id)
    # Upgrade-only fallback for windows enumerated by a pre-0.1.7 binary.
    result = run(["xkill", "-id", window.xid], timeout=3)
    return {
        "ok": result.returncode == 0,
        "schema": "msys.window-action.v1",
        "action": "close",
        "closed": window.xid,
        "window_id": window.xid,
        "title": window.title,
        "returncode": result.returncode,
        "stderr": result.stderr.strip(),
        "compatibility": "xkill",
    }


WINDOW_ACTIONS = frozenset({"focus", "minimize", "move", "resize", "move_resize", "close"})


def _strict_int(payload: Mapping[str, Any], name: str, minimum: int, maximum: int) -> int:
    value = payload.get(name)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be an integer")
    if value < minimum or value > maximum:
        raise ValueError(f"{name} must be in {minimum}..{maximum}")
    return value


def window_action(action: str, window_id: str, payload: Mapping[str, Any] | None = None) -> dict:
    """Execute one handle-addressed native action.

    A handle contains a per-window generation and is verified against the X11
    property before every mutation.  A destroyed XID can therefore never make
    a stale request control a later client that happens to reuse that XID.
    """

    action = str(action).strip().lower().replace("-", "_")
    window_id = str(window_id).strip()
    values = {} if payload is None else dict(payload)
    if action not in WINDOW_ACTIONS:
        return {
            "ok": False,
            "schema": "msys.window-action.v1",
            "reason": "invalid-action",
            "action": action,
        }
    if not window_id.startswith("msys.x11-window.v1:") or len(window_id) >= 192:
        return {
            "ok": False,
            "schema": "msys.window-action.v1",
            "reason": "invalid-window-id",
            "action": action,
            "window_id": window_id,
        }

    option = f"--window-{action.replace('_', '-')}"
    argv = [native_policy_binary(), option, window_id]
    requested: dict[str, int] = {}
    try:
        if action in {"move", "move_resize"}:
            requested["x"] = _strict_int(values, "x", -32768, 32767)
            requested["y"] = _strict_int(values, "y", -32768, 32767)
        if action in {"resize", "move_resize"}:
            requested["width"] = _strict_int(values, "width", 1, 32767)
            requested["height"] = _strict_int(values, "height", 1, 32767)
    except ValueError as exc:
        return {
            "ok": False,
            "schema": "msys.window-action.v1",
            "reason": "invalid-geometry",
            "error": str(exc),
            "action": action,
            "window_id": window_id,
        }
    if action == "move":
        argv.extend([str(requested["x"]), str(requested["y"])])
    elif action == "resize":
        argv.extend([str(requested["width"]), str(requested["height"])])
    elif action == "move_resize":
        argv.extend([
            str(requested["x"]),
            str(requested["y"]),
            str(requested["width"]),
            str(requested["height"]),
        ])
    result = run(argv, timeout=3)
    response: dict[str, Any] = {
        "ok": result.returncode == 0,
        "schema": "msys.window-action.v1",
        "action": action,
        "window_id": window_id,
        "returncode": result.returncode,
    }
    if requested:
        response["requested"] = requested
    if result.stderr.strip():
        response["error"] = result.stderr.strip()
    if result.returncode == 3:
        response["reason"] = "stale-or-missing-window"
    elif result.returncode == 64:
        response["reason"] = "invalid-request"
    elif result.returncode != 0:
        response["reason"] = "x11-action-failed"
    return response


def raise_window(identity: str, title: str, timeout: float = 1.5) -> dict:
    binary = native_policy_binary()
    deadline = time.monotonic() + timeout
    result: subprocess.CompletedProcess[str] | None = None
    while time.monotonic() < deadline:
        if identity:
            argv = [binary, "--raise-window", identity, title]
        else:
            argv = [binary, "--raise-title", title]
        result = run(argv, timeout=min(3, timeout + 0.5))
        if result.returncode == 0:
            break
        time.sleep(0.05)
    if result is None:
        return {"ok": False, "returncode": -1, "stderr": "activation deadline expired"}
    return {
        "ok": result.returncode == 0,
        "returncode": result.returncode,
        "stderr": result.stderr.strip(),
    }


def native_policy_binary() -> str:
    explicit = os.environ.get("MSYS_X11_POLICY_BINARY", "").strip()
    if explicit:
        return explicit

    # Each configured root is authoritative.  In particular, an incomplete
    # installed package must not borrow a binary from a source checkout that
    # happens to contain this module: doing so breaks package isolation and
    # can hide a damaged deployment.
    package_root = os.environ.get("MSYS_PACKAGE_ROOT", "").strip()
    if package_root:
        return str(Path(package_root) / "bin" / "msys-x11-policy")

    session_root = os.environ.get("MSYS_X11_SESSION_ROOT", "").strip()
    if session_root:
        return str(Path(session_root) / "bin" / "msys-x11-policy")

    source_root = Path(__file__).resolve().parents[1]
    return str(source_root / "bin" / "msys-x11-policy")


def display_session_state_path(env: Mapping[str, str] | None = None) -> Path:
    values = os.environ if env is None else env
    explicit = str(values.get("MSYS_DISPLAY_SESSION_STATE_FILE", "")).strip()
    if explicit:
        return Path(explicit)
    runtime = str(values.get("MSYS_RUNTIME_DIR", "")).strip() or "/run/msys/main"
    return Path(runtime) / "display-session.json"


def _matrix_argument(matrix: Sequence[Any]) -> str:
    values: list[str] = []
    for raw in matrix:
        value = float(raw)
        if value == 0:
            value = 0.0
        values.append(format(value, ".12g"))
    return ",".join(values)


def display_session_signature(state: Mapping[str, Any]) -> tuple[Any, ...]:
    geometry = dict(state["geometry"])
    transform = dict(state["input_transform"])
    matrix = transform.get("matrix")
    return (
        state["provider"],
        state["generation"],
        state["display"],
        geometry["width"],
        geometry["height"],
        geometry["depth"],
        transform["enabled"],
        transform.get("mode"),
        transform.get("device"),
        transform.get("space"),
        transform.get("source"),
        transform.get("verified"),
        None if matrix is None else tuple(matrix),
    )


def sync_display_session(state: Mapping[str, Any]) -> None:
    geometry = dict(state["geometry"])
    transform = dict(state["input_transform"])
    enabled = bool(transform["enabled"])
    matrix = transform.get("matrix")
    matrix_argument = _matrix_argument(matrix) if enabled else "none"
    result = run(
        [
            native_policy_binary(),
            "--sync-display-session",
            str(geometry["width"]),
            str(geometry["height"]),
            str(geometry["depth"]),
            "1" if enabled else "0",
            matrix_argument,
        ],
        timeout=3,
    )
    if result.returncode != 0:
        raise RuntimeError(
            result.stderr.strip() or "native display-session sync failed"
        )


@dataclass
class DisplaySessionMonitor:
    path: Path
    expected_display: str
    display_switch_grace: float = 1.0
    _stamp: tuple[int, int, int, int] | None = None
    _signature: tuple[Any, ...] | None = None
    _last_error: str | None = None
    _mismatch_since: float | None = None

    @classmethod
    def from_environment(
        cls, env: Mapping[str, str] | None = None
    ) -> "DisplaySessionMonitor":
        values = os.environ if env is None else env
        try:
            switch_grace = max(
                0.0, min(5.0, float(values.get("MSYS_DISPLAY_SWITCH_GRACE", "1.0")))
            )
        except (TypeError, ValueError):
            switch_grace = 1.0
        return cls(
            path=display_session_state_path(values),
            expected_display=(
                str(values.get("DISPLAY", "")).strip()
                or str(values.get("DISPLAY_ID", "")).strip()
            ),
            display_switch_grace=switch_grace,
        )

    def poll(self, *, force: bool = False) -> dict[str, Any] | None:
        global _active_display_session, _active_display_session_error

        try:
            stat = self.path.stat()
        except FileNotFoundError:
            self._stamp = None
            self._last_error = None
            _active_display_session = None
            _active_display_session_error = "display-session state is unavailable"
            return None
        stamp = (stat.st_dev, stat.st_ino, stat.st_mtime_ns, stat.st_size)
        if not force and stamp == self._stamp:
            return None
        try:
            state = load_state(self.path)
            if self.expected_display and state["display"] != self.expected_display:
                now = time.monotonic()
                if self._mismatch_since is None:
                    self._mismatch_since = now
                    print(
                        "window-policy-agent: observed pending display switch "
                        f"{self.expected_display}->{state['display']}",
                        flush=True,
                    )
                if now - self._mismatch_since < self.display_switch_grace:
                    return None
                raise DisplaySessionChanged(
                    f"display-output moved from {self.expected_display} "
                    f"to {state['display']}"
                )
            self._mismatch_since = None
            signature = display_session_signature(state)
            changed = force or signature != self._signature
            if changed:
                sync_display_session(state)
                self._signature = signature
            self._stamp = stamp
            self._last_error = None
            _active_display_session = state
            _active_display_session_error = None
            return state if changed else None
        except DisplaySessionChanged:
            raise
        except (DisplaySessionError, OSError, RuntimeError, ValueError) as exc:
            message = str(exc)
            if message != self._last_error:
                print(
                    f"window-policy-agent: display-session ignored: {message}",
                    flush=True,
                )
            self._last_error = message
            _active_display_session_error = message
            return None


def _invert_matrix(matrix: Sequence[Any]) -> tuple[float, ...]:
    m = tuple(float(value) for value in matrix)
    determinant = (
        m[0] * (m[4] * m[8] - m[5] * m[7])
        - m[1] * (m[3] * m[8] - m[5] * m[6])
        + m[2] * (m[3] * m[7] - m[4] * m[6])
    )
    if abs(determinant) < 1e-12:
        raise ValueError("input transform is singular")
    inverse_determinant = 1.0 / determinant
    return (
        (m[4] * m[8] - m[5] * m[7]) * inverse_determinant,
        (m[2] * m[7] - m[1] * m[8]) * inverse_determinant,
        (m[1] * m[5] - m[2] * m[4]) * inverse_determinant,
        (m[5] * m[6] - m[3] * m[8]) * inverse_determinant,
        (m[0] * m[8] - m[2] * m[6]) * inverse_determinant,
        (m[2] * m[3] - m[0] * m[5]) * inverse_determinant,
        (m[3] * m[7] - m[4] * m[6]) * inverse_determinant,
        (m[1] * m[6] - m[0] * m[7]) * inverse_determinant,
        (m[0] * m[4] - m[1] * m[3]) * inverse_determinant,
    )


def _transform_point(matrix: Sequence[float], x: float, y: float) -> dict[str, float]:
    scale = matrix[6] * x + matrix[7] * y + matrix[8]
    if abs(scale) < 1e-12:
        raise ValueError("input transform maps navigation region to infinity")
    return {
        "x": round((matrix[0] * x + matrix[1] * y + matrix[2]) / scale, 9),
        "y": round((matrix[3] * x + matrix[4] * y + matrix[5]) / scale, 9),
    }


def navigation_input_region(
    layout: Mapping[str, Any], state: Mapping[str, Any]
) -> dict[str, Any] | None:
    region = layout.get("navigation_region")
    screen = layout.get("screen")
    transform = state.get("input_transform")
    if not isinstance(region, Mapping) or not isinstance(screen, Mapping):
        return None
    if not isinstance(transform, Mapping) or not transform.get("enabled"):
        return None
    width = int(screen["width"])
    height = int(screen["height"])
    inverse = _invert_matrix(transform["matrix"])
    left = int(region["x"]) / width
    top = int(region["y"]) / height
    right = (int(region["x"]) + int(region["width"])) / width
    bottom = (int(region["y"]) + int(region["height"])) / height
    polygon = [
        _transform_point(inverse, left, top),
        _transform_point(inverse, right, top),
        _transform_point(inverse, right, bottom),
        _transform_point(inverse, left, bottom),
    ]
    xs = [point["x"] for point in polygon]
    ys = [point["y"] for point in polygon]
    return {
        "space": "normalized-provider-input",
        "polygon": polygon,
        "bounds": {
            "left": min(xs),
            "top": min(ys),
            "right": max(xs),
            "bottom": max(ys),
        },
    }


def active_display_session() -> tuple[dict[str, Any] | None, str | None]:
    if _active_display_session is not None:
        return _active_display_session, None
    try:
        return load_state(display_session_state_path()), None
    except (DisplaySessionError, OSError, ValueError) as exc:
        return None, _active_display_session_error or str(exc)


def enrich_layout(effective: dict[str, Any]) -> dict[str, Any]:
    state, error = active_display_session()
    result = dict(effective)
    if state is None:
        result["display_session"] = {"state": "unavailable", "error": error or "unknown"}
        result["display_consistent"] = False
        result["navigation_input_region"] = None
        return result
    geometry = state["geometry"]
    screen = effective["screen"]
    expected_display = (
        os.environ.get("DISPLAY") or os.environ.get("DISPLAY_ID") or state["display"]
    )
    consistent = (
        state["display"] == expected_display
        and geometry["width"] == screen["width"]
        and geometry["height"] == screen["height"]
    )
    result["display_session"] = state
    result["display_consistent"] = consistent
    try:
        result["navigation_input_region"] = navigation_input_region(effective, state)
    except (KeyError, TypeError, ValueError) as exc:
        result["navigation_input_region"] = None
        result["display_consistent"] = False
        result["display_session_error"] = str(exc)
    return result


def get_layout() -> dict:
    result = run([native_policy_binary(), "--print-layout"], timeout=3)
    if result.returncode != 0:
        return {
            "ok": False,
            "returncode": result.returncode,
            "error": result.stderr.strip() or "effective layout is unavailable",
        }
    try:
        effective = parse_effective(result.stdout.strip())
    except ValueError as exc:
        return {"ok": False, "returncode": result.returncode, "error": str(exc)}
    return {"ok": True, **enrich_layout(effective)}


def set_layout(payload: dict) -> dict:
    try:
        config = LayoutConfig.from_payload(payload)
    except ValueError as exc:
        return {"ok": False, "reason": "invalid-layout", "error": str(exc)}
    result = run(
        [native_policy_binary(), "--set-layout", *config.command_arguments()],
        timeout=3,
    )
    if result.returncode != 0:
        return {
            "ok": False,
            "returncode": result.returncode,
            "error": result.stderr.strip() or "layout update failed",
        }
    deadline = time.monotonic() + 1.0
    effective: dict = {"ok": False, "error": "layout update was not observed"}
    while time.monotonic() < deadline:
        effective = get_layout()
        if (
            effective.get("ok")
            and effective.get("profile") == config.profile
            and effective.get("orientation_policy") == config.orientation
            and effective.get("insets_policy") == config.insets_wire()
        ):
            break
        time.sleep(0.02)
    return {**effective, "requested": config.encode()}


def layout_changed_payload(layout: Mapping[str, Any]) -> dict[str, Any]:
    session = layout.get("display_session")
    if not isinstance(session, Mapping) or session.get("state") != "ready":
        session = {}
    generation = session.get("generation", 0)
    if isinstance(generation, bool) or not isinstance(generation, int):
        generation = 0
    return {
        "schema": "msys.layout.changed.v1",
        "profile": layout["profile"],
        "orientation_policy": layout["orientation_policy"],
        "orientation": layout["orientation"],
        "geometry": layout["screen"],
        "insets": layout["insets"],
        "workarea": layout["workarea"],
        "navigation_edge": layout["navigation_edge"],
        "navigation_region": layout.get("navigation_region"),
        "display": session.get("display"),
        "display_provider": session.get("provider"),
        "display_generation": generation,
        "display_consistent": bool(layout.get("display_consistent", False)),
    }


def emit_layout_changed(client: MsysClient, layout: Mapping[str, Any]) -> bool:
    if not layout.get("ok"):
        return False
    client.event("msys.layout.changed", layout_changed_payload(layout))
    return True


def emit_method_events(
    client: MsysClient, method: str, result: Mapping[str, Any]
) -> bool:
    if method != "set_layout":
        return False
    return emit_layout_changed(client, result)


def overlay_role(window: XWindow) -> str:
    """Resolve only dismissible roles; titles are an explicit legacy bridge."""

    if window.role in {
        "screen-shield",
        "chooser",
        "notification-center",
        "task-switcher",
        "input-method",
    }:
        return window.role
    if window.role or window.identity:
        return ""
    legacy = (
        ("MSYS Screen Shield", "screen-shield"),
        ("MSYS Intent Chooser", "chooser"),
        ("MSYS Notification Center", "notification-center"),
        ("MSYS Recents", "task-switcher"),
    )
    for prefix, role in legacy:
        if window.title.startswith(prefix):
            return role
    return ""


def dismiss_top_overlay() -> dict[str, Any] | None:
    """Dismiss exactly one top-most overlay through its typed role provider."""

    visible = top_dismissible_window()
    if visible is None:
        return None
    role = overlay_role(visible)
    if not role:
        return None
    method = "cancel_choice" if role == "chooser" else "hide"
    timeout = 2 if role == "chooser" else 3
    try:
        response = MsysClient.public_call(f"role:{role}", method, {}, timeout=timeout)
    except Exception as exc:
        return {
            "ok": False,
            "reason": "overlay-dismiss-failed",
            "dismissed": role,
            "window_id": visible.window_id or visible.xid,
            "error": str(exc),
        }
    if not isinstance(response, dict) or response.get("type") != "return":
        return {
            "ok": False,
            "reason": "overlay-dismiss-failed",
            "dismissed": role,
            "window_id": visible.window_id or visible.xid,
            "error": (
                response.get("message") or response.get("code") or "role call failed"
                if isinstance(response, dict)
                else "role call returned a non-object"
            ),
        }
    payload = response.get("payload", {})
    if not isinstance(payload, dict):
        payload = {}
    if role == "chooser" and not payload.get("cancelled"):
        # The chooser may have disappeared between the X11 snapshot and the
        # role call. It is then safe for Back to continue to the next layer.
        return None
    return {
        "ok": True,
        "dismissed": role,
        "window_id": visible.window_id or visible.xid,
        "compatibility": "legacy-title" if visible.compatibility_title or not visible.role else None,
        "overlay": payload,
    }


def _requested_window_id(payload: Mapping[str, Any]) -> str:
    return str(payload.get("window_id") or payload.get("id") or "").strip()


def managed_component_for_window(
    target: XWindow, managed: Sequence[Mapping[str, Any]]
) -> str | None:
    """Resolve a native surface to its Core-owned component.

    Component identifiers are case-sensitive and authoritative.  Some X11
    clients cannot expose ``MSYS_COMPONENT_ID`` on their top-level surface, so
    stable window identity is the only permitted fallback and is compared
    case-insensitively.  Titles are deliberately excluded: they are mutable,
    localised presentation text and cannot establish process ownership.
    """

    if target.component:
        for item in managed:
            component = item.get("component")
            if isinstance(component, str) and component == target.component:
                return component

    identity = target.identity.casefold()
    if identity:
        for item in managed:
            component = item.get("component")
            candidate_identity = item.get("identity")
            if (
                isinstance(component, str)
                and component
                and isinstance(candidate_identity, str)
                and candidate_identity.casefold() == identity
            ):
                return component
    return None


def handle_window_action(method: str, payload: Mapping[str, Any]) -> dict[str, Any] | None:
    aliases = {
        "focus_window": "focus",
        "minimize_window": "minimize",
        "move_window": "move",
        "resize_window": "resize",
        "move_resize_window": "move_resize",
        "close_window": "close",
    }
    if method == "window_action":
        action = str(payload.get("action") or "").strip().lower().replace("-", "_")
    else:
        action = aliases.get(method, "")
    if not action:
        return None
    window_id = _requested_window_id(payload)
    if action == "close" and window_id.startswith("msys.x11-window.v1:"):
        # Keep supervised component lifecycle authoritative. A direct X11
        # close is reserved for external/identity-less clients.
        try:
            windows = list_windows()
            target = next(
                (window for window in windows if window.window_id == window_id),
                None,
            )
            managed = core_foreground_windows()
        except Exception:
            target = None
            managed = []
        if target is not None and target.kind == "application":
            component = managed_component_for_window(target, managed)
            if component is not None:
                try:
                    response = MsysClient.public_call(
                        "msys.core",
                        "stop",
                        {"component": component},
                        timeout=4,
                    )
                except Exception as exc:
                    response = {
                        "type": "error",
                        "code": "COMPONENT_STOP_FAILED",
                        "message": str(exc),
                    }
                if response.get("type") != "return":
                    return {
                        "ok": False,
                        "schema": "msys.window-action.v1",
                        "action": "close",
                        "window_id": window_id,
                        "component": component,
                        "reason": "component-stop-failed",
                        "error": response.get("message") or response.get("code") or "stop failed",
                    }
                return {
                    "ok": True,
                    "schema": "msys.window-action.v1",
                    "action": "close",
                    "window_id": window_id,
                    "closed_component": component,
                }
    return window_action(action, window_id, payload)


def activate_launcher_home() -> dict[str, Any]:
    """Resolve and activate the selected launcher through Core."""

    try:
        response = MsysClient.public_call(
            "msys.core",
            "activate_role",
            {"role": "launcher"},
            timeout=8,
        )
    except Exception as exc:
        return {
            "ok": False,
            "reason": "launcher-activation-failed",
            "error": str(exc),
        }
    if not isinstance(response, dict):
        return {
            "ok": False,
            "reason": "launcher-activation-failed",
            "error": "activate_role returned a non-object",
        }
    if response.get("type") != "return":
        return {
            "ok": False,
            "reason": "launcher-activation-failed",
            "role_activation": response,
        }
    result = response.get("payload", {})
    if not isinstance(result, dict):
        return {
            "ok": False,
            "reason": "launcher-activation-failed",
            "error": "activate_role returned a non-object payload",
        }
    activation = result.get("activation", {})
    if not isinstance(activation, dict):
        activation = {}
    return {
        "ok": result.get("ok") is not False and activation.get("ok") is not False,
        "role": "launcher",
        "provider": result.get("provider"),
        "generation": result.get("generation"),
        "state": result.get("state"),
        "activation": activation,
    }


def complete_managed_back(component: str, title: str) -> dict[str, Any]:
    """Restore the next Core surface, or Home, after a successful stop.

    This transaction must run outside the provider's private-channel reader.
    Both ``start`` and ``activate_role`` can call ``activate_component`` back
    on that channel before their public response is returned.
    """

    base: dict[str, Any] = {
        "closed_component": component,
        "title": title,
    }
    try:
        remaining = core_foreground_windows()
    except Exception as exc:
        return {
            **base,
            "ok": False,
            "reason": "foreground-refresh-failed",
            "error": str(exc),
        }

    if remaining:
        raw_component = remaining[0].get("component")
        if not isinstance(raw_component, str) or not raw_component:
            return {
                **base,
                "ok": False,
                "reason": "invalid-foreground-component",
            }
        restored_component = raw_component
        try:
            response = MsysClient.public_call(
                "msys.core",
                "start",
                {"component": restored_component},
                timeout=8,
            )
        except Exception as exc:
            response = {
                "type": "error",
                "code": "COMPONENT_RESTORE_FAILED",
                "message": str(exc),
            }
        if not isinstance(response, dict) or response.get("type") != "return":
            return {
                **base,
                "ok": False,
                "destination": "component",
                "restored_component": restored_component,
                "reason": "component-restore-failed",
                "error": (
                    response.get("message") or response.get("code") or "start failed"
                    if isinstance(response, dict)
                    else "start returned a non-object"
                ),
            }
        raw_restoration = response.get("payload", {})
        restoration = (
            dict(raw_restoration) if isinstance(raw_restoration, Mapping) else {}
        )
        activation = restoration.get("activation", {})
        if not isinstance(activation, Mapping):
            activation = {}
        activation_error = restoration.get("activation_error")
        restored = activation.get("ok") is not False and not activation_error
        result = {
            **base,
            "ok": restored,
            "destination": "component",
            "restored_component": restored_component,
            "restoration": restoration,
        }
        if not restored:
            result["reason"] = "component-activation-failed"
        return result

    home = activate_launcher_home()
    return {
        **base,
        "ok": home.get("ok") is True,
        "destination": "home",
        "home": home,
        **({} if home.get("ok") is True else {"reason": "home-activation-failed"}),
    }


def handle_method(method: str, payload: dict) -> dict:
    print(f"window-policy-agent: handle {method}", flush=True)
    if method in {"list_windows", "recents"}:
        native = list_windows()
        try:
            managed = core_foreground_windows()
        except Exception as exc:
            print(f"window-policy-agent: foreground stack unavailable: {exc}", flush=True)
            managed = []
        return {
            "schema": "msys.window-list.v1",
            "windows": merge_managed_windows(managed, native),
        }
    if method in {"get_layout", "layout"}:
        return get_layout()
    if method in {"get_display_session", "display_session"}:
        state, error = active_display_session()
        if state is None:
            return {"ok": False, "reason": "display-session-unavailable", "error": error}
        return {"ok": True, "display_session": state}
    if method == "set_layout":
        return set_layout(payload)
    action_result = handle_window_action(method, payload)
    if action_result is not None:
        return action_result
    if method in {"navigate", "navigation_action"}:
        action = str(payload.get("action") or "").strip().lower()
        if action == "apps":
            action = "recents"
        if action not in {"back", "home", "recents"}:
            return {
                "ok": False,
                "schema": "msys.navigation-action.v1",
                "reason": "invalid-navigation-action",
                "action": action,
            }
        if action == "recents":
            response = MsysClient.public_call(
                "role:task-switcher",
                "show",
                {},
                timeout=7,
            )
            if not isinstance(response, dict) or response.get("type") != "return":
                delegated = {
                    "ok": False,
                    "reason": "task-switcher-unavailable",
                    "error": (
                        response.get("message") or response.get("code") or "show failed"
                        if isinstance(response, dict)
                        else "show returned a non-object"
                    ),
                }
            else:
                role_payload = response.get("payload", {})
                delegated = dict(role_payload) if isinstance(role_payload, dict) else {"ok": False}
        else:
            delegated = handle_method(action, {})
        return {
            **delegated,
            "schema": "msys.navigation-action.v1",
            "action": action,
            "input": str(payload.get("input") or "button"),
        }
    if method in {"close_active", "back"}:
        overlay = dismiss_top_overlay()
        if overlay is not None:
            return overlay
        visible = top_content_window()
        try:
            managed = core_foreground_windows()
        except Exception as exc:
            print(f"window-policy-agent: foreground stack unavailable: {exc}", flush=True)
            managed = []
        if managed:
            if visible and (
                visible.role == "launcher"
                or (
                    not visible.role
                    and not visible.identity
                    and visible.title.startswith("MSYS Launcher")
                )
            ):
                return {"ok": False, "reason": "home-visible"}
            active = managed[0]
            component = str(active["component"])
            try:
                response = MsysClient.public_call(
                    "msys.core",
                    "stop",
                    {"component": component},
                    timeout=4,
                )
            except Exception as exc:
                response = {
                    "type": "error",
                    "code": "COMPONENT_STOP_FAILED",
                    "message": str(exc),
                }
            if not isinstance(response, dict) or response.get("type") != "return":
                return {
                    "ok": False,
                    "component": component,
                    "reason": "component-stop-failed",
                    "error": (
                        response.get("message") or response.get("code") or "stop failed"
                        if isinstance(response, dict)
                        else "stop returned a non-object"
                    ),
                }
            return complete_managed_back(
                component,
                str(active.get("title") or component),
            )
        window = active_or_top_user_window()
        if not window:
            return {"ok": False, "reason": "no-user-window"}
        return {"ok": True, **close_window(window)}
    if method == "activate_component":
        identity = str(payload.get("identity", "")).strip()
        title = str(payload.get("title", "")).strip()
        if not title and not identity:
            return {"ok": False, "reason": "missing-window-identity-and-title"}
        return {
            "component": str(payload.get("component", "")),
            "identity": identity,
            "title": title,
            **raise_window(identity, title),
        }
    if method == "home":
        return activate_launcher_home()
    return {"ok": False, "error": f"unknown method {method}"}


def start_native_policy() -> subprocess.Popen[str]:
    """Start the prebuilt native policy; never invoke a compiler at runtime."""

    binary = native_policy_binary()
    if not Path(binary).is_file() or not os.access(binary, os.X_OK):
        raise FileNotFoundError(
            f"prebuilt native policy is unavailable or not executable: {binary}; "
            "run `make all` for a source checkout or `make package` before install"
        )
    env = os.environ.copy()
    env.setdefault("DISPLAY", env.get("DISPLAY_ID", ":0"))
    # The compatibility Python wrapper owns the inherited private mIPC fd.
    # Its in-process X11 helper must remain policy-only or both processes would
    # try to handshake on the same component channel.
    env["MSYS_X11_NATIVE_AGENT"] = "0"
    return subprocess.Popen([binary], env=env, text=True)


def _reply_method_call(client: MsysClient, message: Mapping[str, Any]) -> None:
    request_id = int(message.get("id", 0))
    method = str(message.get("method", ""))
    try:
        payload = handle_method(method, dict(message.get("payload", {})))
    except Exception as exc:
        client.send({
            "type": "error",
            "id": request_id,
            "code": "WINDOW_POLICY_ERROR",
            "message": str(exc),
        })
        return
    client.send({"type": "return", "id": request_id, "payload": payload})
    try:
        emit_method_events(client, method, payload)
    except Exception as exc:
        print(
            f"window-policy-agent: post-return broadcast failed: {exc}",
            flush=True,
        )


def dispatch_method_call(
    client: MsysClient,
    message: Mapping[str, Any],
) -> threading.Thread | None:
    """Reply to one provider call while keeping navigation reentrant.

    ``activate_role(launcher)`` calls back into this provider's
    ``activate_component`` method, and task-switcher ``show`` calls back into
    ``recents``. Back can similarly restore the previous component with
    ``start``, which also calls ``activate_component``. Those navigation
    actions therefore run on a worker while the single private-channel reader
    remains free to service the callback.
    """

    method = str(message.get("method", ""))
    payload = message.get("payload", {})
    navigation_action = ""
    if isinstance(payload, Mapping):
        navigation_action = str(payload.get("action") or "").strip().lower()
    needs_reentrant_worker = method in {"home", "back", "close_active"} or (
        method in {"navigate", "navigation_action"}
        and navigation_action in {"back", "home", "recents", "apps"}
    )
    if not needs_reentrant_worker:
        _reply_method_call(client, message)
        return None
    worker = threading.Thread(
        target=_reply_method_call,
        args=(client, dict(message)),
        name=f"msys-window-policy-reentrant:{message.get('id', 0)}",
        daemon=True,
    )
    worker.start()
    return worker


def main() -> int:
    native = start_native_policy()
    client = MsysClient.from_env()
    monitor = DisplaySessionMonitor.from_environment()
    try:
        initial_session = monitor.poll(force=True)
    except DisplaySessionChanged as exc:
        print(f"window-policy-agent: {exc}; restarting policy", flush=True)
        native.terminate()
        try:
            native.wait(timeout=2)
        except subprocess.TimeoutExpired:
            native.kill()
        return 75
    client.hello()
    client.ready()
    client.event("msys.window_policy.ready", {"component": client.component_id})
    if initial_session is not None:
        client.event(
            "msys.display_session.applied",
            {
                "provider": initial_session["provider"],
                "generation": initial_session["generation"],
                "display": initial_session["display"],
                "geometry": initial_session["geometry"],
            },
        )

    try:
        while True:
            msg = client.recv(timeout=0.2)
            if native.poll() is not None:
                return native.returncode or 0
            try:
                changed_session = monitor.poll()
            except DisplaySessionChanged as exc:
                print(f"window-policy-agent: {exc}; restarting policy", flush=True)
                return 75
            if changed_session is not None:
                client.event(
                    "msys.display_session.applied",
                    {
                        "provider": changed_session["provider"],
                        "generation": changed_session["generation"],
                        "display": changed_session["display"],
                        "geometry": changed_session["geometry"],
                    },
                )
            if not msg:
                continue
            print(
                "window-policy-agent: recv "
                f"type={msg.get('type')} method={msg.get('method')} id={msg.get('id')}",
                flush=True,
            )
            if msg.get("type") in {"shutdown", "eof"}:
                return 0
            if msg.get("type") != "call":
                continue
            dispatch_method_call(client, msg)
    finally:
        if native.poll() is None:
            native.terminate()
            try:
                native.wait(timeout=2)
            except subprocess.TimeoutExpired:
                native.send_signal(signal.SIGKILL)
        time.sleep(0.05)


if __name__ == "__main__":
    raise SystemExit(main())
