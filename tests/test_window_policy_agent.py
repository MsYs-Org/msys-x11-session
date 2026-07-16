from __future__ import annotations

import json
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from msys_x11_session import window_policy_agent as agent
from msys_x11_session.display_session import write_state


def display_state(
    *,
    provider: str,
    display: str,
    width: int,
    height: int,
    matrix: list[int] | None,
) -> dict:
    enabled = matrix is not None
    return {
        "schema": "msys.display-session.v1",
        "state": "ready",
        "provider": provider,
        "generation": 3,
        "display": display,
        "geometry": {"width": width, "height": height, "depth": 24},
        "input_transform": {
            "enabled": enabled,
            "mode": "ch347-direct" if enabled else "none",
            "device": "CH347 XPT2046" if enabled else None,
            "space": "normalized-display",
            "matrix": matrix,
            "source": "ch347-direct-effective" if enabled else "no-provider-owned-input",
            "verified": True,
        },
        "observed_at_unix_ms": 123,
    }


class WindowPolicyIdentityTests(unittest.TestCase):
    def test_native_window_list_exposes_opaque_generation_checked_ids(self) -> None:
        document = {
            "schema": "msys.window-list.v1",
            "windows": [{
                "schema": "msys.window.v1",
                "id": "msys.x11-window.v1:session-7:0x2a",
                "native_id": "0x2A",
                "title": "Visor",
                "identity": "org.example.viewer",
                "component": "org.example.viewer:main",
                "role": "application",
                "kind": "application",
                "state": "minimized",
                "geometry": {"x": 4, "y": 8, "width": 300, "height": 200},
                "compatibility_title": False,
            }],
        }
        windows = agent.parse_native_window_list(json.dumps(document))
        self.assertEqual(len(windows), 1)
        self.assertEqual(windows[0].window_id, "msys.x11-window.v1:session-7:0x2a")
        self.assertEqual(windows[0].xid, "0x2a")
        self.assertEqual(windows[0].state, "minimized")
        self.assertEqual(windows[0].geometry["width"], 300)
        self.assertEqual(windows[0].descriptor()["component"], "org.example.viewer:main")

    @mock.patch.object(agent, "run")
    def test_native_window_list_is_preferred_over_xwininfo(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess(
            [],
            0,
            json.dumps({
                "schema": "msys.window-list.v1",
                "windows": [{
                    "id": "msys.x11-window.v1:s:0x1",
                    "native_id": "0x1",
                    "title": "App",
                    "role": "application",
                    "kind": "application",
                    "state": "visible",
                }],
            }),
            "",
        )
        windows = agent.list_windows()
        self.assertEqual(windows[0].window_id, "msys.x11-window.v1:s:0x1")
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[0][-1], "--list-windows")

    def test_managed_and_native_windows_merge_by_component_not_title(self) -> None:
        native = [agent.XWindow(
            "0x4",
            "Titre localis\u00e9",
            window_id="msys.x11-window.v1:s:0x4",
            identity="org.example.viewer",
            component="org.example.viewer:main",
        )]
        merged = agent.merge_managed_windows([{
            "component": "org.example.viewer:main",
            "identity": "org.example.viewer",
            "title": "Viewer",
            "state": "ready",
        }], native)
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0]["id"], "msys.x11-window.v1:s:0x4")
        self.assertEqual(merged[0]["title"], "Titre localis\u00e9")
        self.assertTrue(merged[0]["managed"])

    @mock.patch.object(agent, "run")
    def test_typed_move_resize_uses_opaque_id_and_strict_geometry(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess([], 0, "", "")
        result = agent.window_action(
            "move_resize",
            "msys.x11-window.v1:s:0x4",
            {"x": -10, "y": 12, "width": 640, "height": 360},
        )
        self.assertTrue(result["ok"])
        self.assertEqual(run.call_args.args[0][-6:], [
            "--window-move-resize",
            "msys.x11-window.v1:s:0x4",
            "-10",
            "12",
            "640",
            "360",
        ])
        rejected = agent.window_action(
            "resize",
            "msys.x11-window.v1:s:0x4",
            {"width": True, "height": 100},
        )
        self.assertEqual(rejected["reason"], "invalid-geometry")
        self.assertEqual(run.call_count, 1)

    @mock.patch.object(agent, "run")
    def test_stale_window_action_is_typed(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess([], 3, "", "stale")
        result = agent.window_action("focus", "msys.x11-window.v1:old:0x9")
        self.assertFalse(result["ok"])
        self.assertEqual(result["reason"], "stale-or-missing-window")

    @mock.patch.object(agent, "window_action")
    @mock.patch.object(agent.MsysClient, "public_call")
    @mock.patch.object(agent, "core_foreground_windows")
    @mock.patch.object(agent, "list_windows")
    def test_close_window_preserves_managed_component_lifecycle(
        self,
        list_windows: mock.Mock,
        foreground: mock.Mock,
        call: mock.Mock,
        native_action: mock.Mock,
    ) -> None:
        window_id = "msys.x11-window.v1:s:0x22"
        list_windows.return_value = [agent.XWindow(
            "0x22",
            "Viewer",
            window_id=window_id,
            identity="org.example.viewer",
            component="org.example.viewer:main",
        )]
        foreground.return_value = [
            {
                "component": "org.example.other:main",
                "identity": "org.example.viewer",
                "title": "Identity collision",
            },
            {
                "component": "org.example.viewer:main",
                "identity": "org.example.unrelated",
                "title": "Different title",
            },
        ]
        call.return_value = {"type": "return", "payload": {"state": "stopped"}}
        result = agent.handle_method("close_window", {"window_id": window_id})
        self.assertTrue(result["ok"])
        self.assertEqual(result["closed_component"], "org.example.viewer:main")
        call.assert_called_once_with(
            "msys.core",
            "stop",
            {"component": "org.example.viewer:main"},
            timeout=4,
        )
        native_action.assert_not_called()

    @mock.patch.object(agent, "window_action")
    @mock.patch.object(agent.MsysClient, "public_call")
    @mock.patch.object(agent, "core_foreground_windows")
    @mock.patch.object(agent, "list_windows")
    def test_close_window_matches_identity_only_case_insensitively(
        self,
        list_windows: mock.Mock,
        foreground: mock.Mock,
        call: mock.Mock,
        native_action: mock.Mock,
    ) -> None:
        window_id = "msys.x11-window.v1:s:0x23"
        list_windows.return_value = [agent.XWindow(
            "0x23",
            "Calculator",
            window_id=window_id,
            identity="ORG.MSYS.APPS.CALCULATOR",
        )]
        foreground.return_value = [{
            "component": "org.msys.apps:calculator",
            "identity": "org.msys.apps.calculator",
            "title": "Calculator",
        }]
        call.return_value = {"type": "return", "payload": {"state": "stopped"}}

        result = agent.handle_method("close_window", {"window_id": window_id})

        self.assertTrue(result["ok"])
        self.assertEqual(result["closed_component"], "org.msys.apps:calculator")
        call.assert_called_once_with(
            "msys.core",
            "stop",
            {"component": "org.msys.apps:calculator"},
            timeout=4,
        )
        native_action.assert_not_called()

    @mock.patch.object(agent, "window_action")
    @mock.patch.object(agent.MsysClient, "public_call")
    @mock.patch.object(agent, "core_foreground_windows")
    @mock.patch.object(agent, "list_windows")
    def test_close_window_does_not_match_managed_component_by_title(
        self,
        list_windows: mock.Mock,
        foreground: mock.Mock,
        call: mock.Mock,
        native_action: mock.Mock,
    ) -> None:
        window_id = "msys.x11-window.v1:s:0x24"
        list_windows.return_value = [agent.XWindow(
            "0x24",
            "Calculator",
            window_id=window_id,
            identity="org.external.calculator",
            component="ORG.MSYS.APPS:CALCULATOR",
        )]
        foreground.return_value = [{
            "component": "org.msys.apps:calculator",
            "identity": "org.msys.apps.calculator",
            "title": "Calculator",
        }]
        native_action.return_value = {
            "ok": True,
            "schema": "msys.window-action.v1",
            "action": "close",
            "window_id": window_id,
        }

        result = agent.handle_method("close_window", {"window_id": window_id})

        self.assertTrue(result["ok"])
        native_action.assert_called_once_with("close", window_id, {"window_id": window_id})
        call.assert_not_called()

    @mock.patch.object(agent, "run")
    def test_visibility_parser_accepts_xwininfo_spacing(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess([], 0, "  Map State: IsViewable\n", "")
        self.assertTrue(agent.is_visible_window(agent.XWindow("0x1", "App")))
        run.return_value = subprocess.CompletedProcess([], 0, "  Map State: IsUnMapped\n", "")
        self.assertFalse(agent.is_visible_window(agent.XWindow("0x1", "App")))

    @mock.patch.object(agent, "run")
    def test_raise_uses_identity_with_title_as_compatibility_fallback(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess([], 0, "", "")
        result = agent.raise_window("org.example.viewer", "Translated Viewer")
        self.assertTrue(result["ok"])
        argv = run.call_args.args[0]
        self.assertEqual(argv[-3:], ["--raise-window", "org.example.viewer", "Translated Viewer"])

    @mock.patch.object(agent, "run")
    def test_raise_title_remains_available_for_legacy_external_window(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess([], 0, "", "")
        agent.raise_window("", "Legacy Window")
        argv = run.call_args.args[0]
        self.assertEqual(argv[-2:], ["--raise-title", "Legacy Window"])

    @mock.patch.object(agent, "raise_window")
    def test_activate_component_forwards_stable_identity(self, raise_window: mock.Mock) -> None:
        raise_window.return_value = {"ok": True, "returncode": 0, "stderr": ""}
        result = agent.handle_method(
            "activate_component",
            {
                "component": "org.example.viewer:main",
                "identity": "org.example.viewer",
                "title": "Viewer",
            },
        )
        raise_window.assert_called_once_with("org.example.viewer", "Viewer")
        self.assertTrue(result["ok"])

    @mock.patch.object(agent, "raise_window")
    @mock.patch.object(agent.MsysClient, "public_call")
    def test_home_activates_selected_launcher_role_without_local_identity_guess(
        self,
        public_call: mock.Mock,
        raise_window: mock.Mock,
    ) -> None:
        public_call.return_value = {
            "type": "return",
            "payload": {
                "ok": True,
                "role": "launcher",
                "provider": "org.vendor.nebula:home",
                "generation": 9,
                "state": "ready",
                "activation": {
                    "ok": True,
                    "identity": "org.vendor.nebula.home",
                    "title": "Nebula Home",
                },
            },
        }

        result = agent.handle_method("home", {})

        public_call.assert_called_once_with(
            "msys.core",
            "activate_role",
            {"role": "launcher"},
            timeout=8,
        )
        raise_window.assert_not_called()
        self.assertTrue(result["ok"])
        self.assertEqual(result["provider"], "org.vendor.nebula:home")
        self.assertEqual(result["activation"]["title"], "Nebula Home")

    @mock.patch.object(agent.MsysClient, "public_call")
    def test_home_preserves_typed_role_activation_failure(self, public_call: mock.Mock) -> None:
        public_call.return_value = {
            "type": "error",
            "code": "UNKNOWN_ROLE",
            "message": "launcher",
        }

        result = agent.handle_method("home", {})

        self.assertFalse(result["ok"])
        self.assertEqual(result["role_activation"]["code"], "UNKNOWN_ROLE")

    def test_home_dispatch_keeps_reader_free_for_activate_component_callback(self) -> None:
        entered = threading.Event()
        release = threading.Event()

        class Client:
            def __init__(self) -> None:
                self.packets: list[dict] = []

            def send(self, message: dict) -> None:
                self.packets.append(message)

        client = Client()

        def blocked_home(_method: str, _payload: dict) -> dict:
            entered.set()
            self.assertTrue(release.wait(timeout=2))
            return {"ok": True, "provider": "org.vendor.nebula:home"}

        with mock.patch.object(agent, "handle_method", side_effect=blocked_home):
            worker = agent.dispatch_method_call(client, {
                "type": "call",
                "id": 77,
                "method": "home",
                "payload": {},
            })
            self.assertIsNotNone(worker)
            self.assertTrue(entered.wait(timeout=1))
            self.assertTrue(worker.is_alive())
            self.assertEqual(client.packets, [])
            release.set()
            worker.join(timeout=2)

        self.assertFalse(worker.is_alive())
        self.assertEqual(client.packets, [{
            "type": "return",
            "id": 77,
            "payload": {"ok": True, "provider": "org.vendor.nebula:home"},
        }])

    def test_recents_navigation_dispatch_keeps_reader_free_for_callback(self) -> None:
        entered = threading.Event()
        release = threading.Event()
        client = mock.Mock()

        def blocked_reply(_client: object, _message: object) -> None:
            entered.set()
            self.assertTrue(release.wait(timeout=2))

        with mock.patch.object(agent, "_reply_method_call", side_effect=blocked_reply):
            worker = agent.dispatch_method_call(client, {
                "type": "call",
                "id": 78,
                "method": "navigation_action",
                "payload": {"action": "apps"},
            })
            self.assertIsNotNone(worker)
            self.assertTrue(entered.wait(timeout=1))
            self.assertTrue(worker.is_alive())
            release.set()
            worker.join(timeout=2)
        self.assertFalse(worker.is_alive())

    def test_back_dispatch_keeps_reader_free_for_restore_callbacks(self) -> None:
        requests = [
            {"type": "call", "id": 79, "method": "back", "payload": {}},
            {"type": "call", "id": 80, "method": "close_active", "payload": {}},
            {
                "type": "call",
                "id": 81,
                "method": "navigation_action",
                "payload": {"action": "back", "input": "swipe"},
            },
        ]
        for request in requests:
            with self.subTest(method=request["method"]):
                entered = threading.Event()
                release = threading.Event()
                client = mock.Mock()

                def blocked_reply(_client: object, _message: object) -> None:
                    entered.set()
                    self.assertTrue(release.wait(timeout=2))

                with mock.patch.object(
                    agent, "_reply_method_call", side_effect=blocked_reply
                ):
                    worker = agent.dispatch_method_call(client, request)
                    self.assertIsNotNone(worker)
                    self.assertTrue(entered.wait(timeout=1))
                    self.assertTrue(worker.is_alive())
                    release.set()
                    worker.join(timeout=2)
                self.assertFalse(worker.is_alive())

    def test_chooser_hosts_are_not_treated_as_user_apps(self) -> None:
        parsed = [
            agent.XWindow("0x1", "MSYS Intent Chooser"),
            agent.XWindow("0x2", "msys-intent-chooser-host"),
            agent.XWindow("0x3", "Actual App"),
        ]
        with mock.patch.object(agent, "list_windows", return_value=parsed), mock.patch.object(
            agent, "is_visible_window", return_value=True
        ):
            self.assertEqual([window.xid for window in agent.user_windows()], ["0x3"])

    def test_input_method_is_never_treated_as_user_content(self) -> None:
        keyboard = agent.XWindow(
            "0x10",
            "Vendor keyboard",
            identity="org.vendor.keyboard",
            role="input-method",
            kind="overlay",
        )
        application = agent.XWindow("0x11", "Actual App")
        with mock.patch.object(
            agent, "list_windows", return_value=[keyboard, application]
        ), mock.patch.object(agent, "is_visible_window", return_value=True):
            self.assertEqual(agent.user_windows(), [application])
            self.assertIs(agent.top_content_window(), application)

    def test_higher_overlay_precedes_input_method_for_back(self) -> None:
        task_switcher = agent.XWindow(
            "0x12", "Tasks", role="task-switcher", kind="overlay"
        )
        keyboard = agent.XWindow(
            "0x13", "Keyboard", role="input-method", kind="overlay"
        )
        with mock.patch.object(
            agent, "list_windows", return_value=[task_switcher, keyboard]
        ), mock.patch.object(agent, "is_visible_window", return_value=True):
            self.assertIs(agent.top_dismissible_window(), task_switcher)

    def test_dismissible_selection_ignores_hidden_provider_hosts(self) -> None:
        windows = [
            agent.XWindow("0x1", "msys-notification-center-host"),
            agent.XWindow("0x2", "MSYS Intent Chooser"),
            agent.XWindow("0x3", "Actual App"),
        ]
        with mock.patch.object(agent, "list_windows", return_value=windows), mock.patch.object(
            agent, "is_visible_window", side_effect=lambda window: window.xid != "0x1"
        ):
            self.assertEqual(agent.top_dismissible_window().xid, "0x2")

    @mock.patch.object(agent.MsysClient, "public_call")
    @mock.patch.object(agent, "list_windows")
    def test_back_dismisses_only_topmost_typed_overlay(
        self, list_windows: mock.Mock, call: mock.Mock
    ) -> None:
        list_windows.return_value = [
            agent.XWindow(
                "0x8",
                "Vendor shield",
                window_id="msys.x11-window.v1:s:0x8",
                identity="org.vendor.shield",
                role="screen-shield",
                kind="overlay",
            ),
            agent.XWindow(
                "0x7",
                "Vendor chooser",
                window_id="msys.x11-window.v1:s:0x7",
                identity="org.vendor.chooser",
                role="chooser",
                kind="overlay",
            ),
        ]
        call.return_value = {
            "type": "return",
            "payload": {"visible": False, "changed": True},
        }
        result = agent.handle_method("back", {})
        self.assertEqual(result["dismissed"], "screen-shield")
        call.assert_called_once_with("role:screen-shield", "hide", {}, timeout=3)

    @mock.patch.object(agent.MsysClient, "public_call")
    @mock.patch.object(agent, "list_windows")
    def test_back_hides_input_method_before_application(
        self, list_windows: mock.Mock, call: mock.Mock
    ) -> None:
        list_windows.return_value = [agent.XWindow(
            "0x14",
            "Vendor keyboard",
            window_id="msys.x11-window.v1:s:0x14",
            identity="org.vendor.keyboard",
            role="input-method",
            kind="overlay",
        )]
        call.return_value = {
            "type": "return",
            "payload": {"visible": False, "changed": True},
        }

        result = agent.handle_method("back", {})

        self.assertTrue(result["ok"])
        self.assertEqual(result["dismissed"], "input-method")
        call.assert_called_once_with(
            "role:input-method",
            "hide",
            {"reason": "navigation-back", "restore_target": False},
            timeout=3,
        )

    @mock.patch.object(agent.MsysClient, "public_call")
    @mock.patch.object(agent, "list_windows")
    def test_input_method_hide_failure_is_fail_closed(
        self, list_windows: mock.Mock, call: mock.Mock
    ) -> None:
        list_windows.return_value = [agent.XWindow(
            "0x15",
            "Vendor keyboard",
            window_id="msys.x11-window.v1:s:0x15",
            identity="org.vendor.keyboard",
            role="input-method",
            kind="overlay",
        )]
        call.side_effect = RuntimeError("provider unavailable")

        result = agent.handle_method("close_active", {})

        self.assertFalse(result["ok"])
        self.assertEqual(result["reason"], "overlay-dismiss-failed")
        self.assertEqual(result["dismissed"], "input-method")
        call.assert_called_once_with(
            "role:input-method",
            "hide",
            {"reason": "navigation-back", "restore_target": False},
            timeout=3,
        )

    def test_back_overlay_only_does_not_change_application_destination(self) -> None:
        dismissed = {
            "ok": True,
            "dismissed": "task-switcher",
            "window_id": "msys.x11-window.v1:s:0x31",
        }
        with mock.patch.object(
            agent, "dismiss_top_overlay", return_value=dismissed
        ), mock.patch.object(agent, "core_foreground_windows") as foreground, mock.patch.object(
            agent.MsysClient, "public_call"
        ) as call:
            result = agent.handle_method("back", {})

        self.assertEqual(result, dismissed)
        foreground.assert_not_called()
        call.assert_not_called()

    def test_back_from_last_managed_application_activates_home(self) -> None:
        current = {
            "component": "org.msys.apps:calculator",
            "identity": "org.msys.apps.calculator",
            "title": "Calculator",
        }
        window = agent.XWindow(
            "0x41",
            "Calculator",
            window_id="msys.x11-window.v1:s:0x41",
            identity="org.msys.apps.calculator",
            component="org.msys.apps:calculator",
        )
        with mock.patch.object(
            agent, "dismiss_top_overlay", return_value=None
        ), mock.patch.object(
            agent, "top_content_window", return_value=window
        ), mock.patch.object(
            agent, "core_foreground_windows", side_effect=[[current], [current]]
        ) as foreground, mock.patch.object(agent.MsysClient, "public_call") as call:
            call.side_effect = [
                {
                    "type": "return",
                    "payload": {"handled": False, "fallback": True},
                },
                {
                    "type": "return",
                    "payload": {
                        "component": "org.msys.apps:calculator",
                        "state": "background",
                    },
                },
                {
                    "type": "return",
                    "payload": {
                        "ok": True,
                        "role": "launcher",
                        "provider": "org.msys.shell.pyside:launcher",
                        "generation": 4,
                        "state": "ready",
                        "activation": {"ok": True},
                    },
                },
            ]

            with mock.patch.object(
                agent,
                "window_action",
                return_value={
                    "ok": True,
                    "schema": "msys.window-action.v1",
                    "action": "minimize",
                    "window_id": window.window_id,
                },
            ) as action:
                result = agent.handle_method("back", {})

        self.assertTrue(result["ok"])
        self.assertEqual(result["backgrounded_component"], current["component"])
        self.assertEqual(result["destination"], "home")
        self.assertEqual(
            result["home"]["provider"], "org.msys.shell.pyside:launcher"
        )
        self.assertEqual(foreground.call_count, 2)
        action.assert_called_once_with("minimize", window.window_id)
        self.assertEqual(call.call_args_list, [
            mock.call(
                "msys.core",
                "navigation_back",
                {},
                timeout=1.5,
            ),
            mock.call(
                "msys.core",
                "background_component",
                {"component": "org.msys.apps:calculator"},
                timeout=4,
            ),
            mock.call(
                "msys.core",
                "activate_role",
                {"role": "launcher"},
                timeout=8,
            ),
        ])

    def test_back_handled_by_application_does_not_stop_component(self) -> None:
        current = {
            "component": "org.msys.settings:main",
            "identity": "org.msys.settings",
            "title": "Settings",
        }
        with mock.patch.object(
            agent, "dismiss_top_overlay", return_value=None
        ), mock.patch.object(
            agent, "top_content_window", return_value=None
        ), mock.patch.object(
            agent, "core_foreground_windows", return_value=[current]
        ) as foreground, mock.patch.object(agent.MsysClient, "public_call") as call:
            call.return_value = {
                "type": "return",
                "payload": {
                    "handled": True,
                    "fallback": False,
                    "component": current["component"],
                    "result": {"handled": True, "page": "home"},
                },
            }

            result = agent.handle_method("back", {})

        self.assertTrue(result["ok"])
        self.assertEqual(result["destination"], "application")
        foreground.assert_called_once_with()
        call.assert_called_once_with(
            "msys.core",
            "navigation_back",
            {},
            timeout=1.5,
        )

    def test_back_restores_exact_previous_managed_application(self) -> None:
        current = {
            "component": "org.msys.apps:notes",
            "identity": "org.msys.apps.notes",
            "title": "Notes",
        }
        previous = {
            "component": "org.msys.apps:calculator",
            "identity": "org.msys.apps.calculator",
            "title": "Calculator",
        }
        with mock.patch.object(
            agent, "dismiss_top_overlay", return_value=None
        ), mock.patch.object(
            agent, "top_content_window", return_value=None
        ), mock.patch.object(
            agent,
            "core_foreground_windows",
            side_effect=[[current, previous], [previous]],
        ) as foreground, mock.patch.object(agent.MsysClient, "public_call") as call:
            call.side_effect = [
                {
                    "type": "return",
                    "payload": {"component": current["component"], "state": "stopped"},
                },
                {
                    "type": "return",
                    "payload": {
                        "component": previous["component"],
                        "state": "ready",
                        "activation": {
                            "ok": True,
                            "identity": previous["identity"],
                        },
                    },
                },
            ]

            result = agent.handle_method("close_active", {})

        self.assertTrue(result["ok"])
        self.assertEqual(result["destination"], "component")
        self.assertEqual(result["restored_component"], previous["component"])
        self.assertEqual(foreground.call_count, 2)
        self.assertEqual(call.call_args_list, [
            mock.call(
                "msys.core",
                "stop",
                {"component": current["component"]},
                timeout=4,
            ),
            mock.call(
                "msys.core",
                "start",
                {"component": previous["component"]},
                timeout=8,
            ),
        ])

    def test_back_background_failure_does_not_minimize_or_activate_home(self) -> None:
        current = {
            "component": "org.msys.apps:notes",
            "identity": "org.msys.apps.notes",
            "title": "Notes",
        }
        window = agent.XWindow(
            "0x42",
            "Notes",
            window_id="msys.x11-window.v1:s:0x42",
            identity="org.msys.apps.notes",
            component="org.msys.apps:notes",
        )
        with mock.patch.object(
            agent, "dismiss_top_overlay", return_value=None
        ), mock.patch.object(
            agent, "top_content_window", return_value=window
        ), mock.patch.object(
            agent, "core_foreground_windows", side_effect=[[current], [current]]
        ) as foreground, mock.patch.object(
            agent.MsysClient,
            "public_call",
            side_effect=[
                {
                    "type": "return",
                    "payload": {"handled": False, "fallback": True},
                },
                RuntimeError("still running"),
            ],
        ) as call, mock.patch.object(agent, "window_action") as action:
            result = agent.handle_method("back", {})

        self.assertFalse(result["ok"])
        self.assertEqual(result["reason"], "component-background-failed")
        self.assertEqual(foreground.call_count, 2)
        action.assert_not_called()
        self.assertEqual(call.call_args_list, [
            mock.call("msys.core", "navigation_back", {}, timeout=1.5),
            mock.call(
                "msys.core",
                "background_component",
                {"component": current["component"]},
                timeout=4,
            ),
        ])

    def test_back_minimize_failure_rolls_managed_component_forward(self) -> None:
        current = {
            "component": "org.msys.apps:notes",
            "identity": "org.msys.apps.notes",
            "title": "Notes",
        }
        window = agent.XWindow(
            "0x43",
            "Notes",
            window_id="msys.x11-window.v1:s:0x43",
            identity="org.msys.apps.notes",
            component="org.msys.apps:notes",
        )
        with mock.patch.object(
            agent, "dismiss_top_overlay", return_value=None
        ), mock.patch.object(
            agent, "top_content_window", return_value=window
        ), mock.patch.object(
            agent, "core_foreground_windows", side_effect=[[current], [current]]
        ), mock.patch.object(
            agent.MsysClient,
            "public_call",
            side_effect=[
                {"type": "return", "payload": {"handled": False, "fallback": True}},
                {"type": "return", "payload": {"state": "background"}},
                {"type": "return", "payload": {"state": "ready", "activation": {"ok": True}}},
            ],
        ) as call, mock.patch.object(
            agent,
            "window_action",
            return_value={"ok": False, "reason": "stale-or-missing-window"},
        ):
            result = agent.handle_method("back", {})

        self.assertFalse(result["ok"])
        self.assertEqual(result["reason"], "window-minimize-failed")
        self.assertTrue(result["rollback"]["ok"])
        self.assertEqual(call.call_args_list[-1], mock.call(
            "msys.core",
            "start",
            {"component": current["component"]},
            timeout=8,
        ))

    def test_back_minimizes_external_window_without_closing_it(self) -> None:
        window = agent.XWindow(
            "0x44",
            "External",
            window_id="msys.x11-window.v1:s:0x44",
            identity="org.example.external",
        )
        with mock.patch.object(
            agent, "dismiss_top_overlay", return_value=None
        ), mock.patch.object(
            agent, "top_content_window", return_value=window
        ), mock.patch.object(
            agent, "core_foreground_windows", return_value=[]
        ), mock.patch.object(
            agent, "active_or_top_user_window", return_value=window
        ), mock.patch.object(
            agent,
            "window_action",
            return_value={"ok": True, "action": "minimize", "window_id": window.window_id},
        ) as action, mock.patch.object(
            agent,
            "activate_launcher_home",
            return_value={"ok": True, "role": "launcher"},
        ):
            result = agent.handle_method("back", {})

        self.assertTrue(result["ok"])
        self.assertEqual(result["backgrounded_window"], window.window_id)
        action.assert_called_once_with("minimize", window.window_id)

    def test_navigation_action_gives_buttons_and_swipes_one_typed_entrypoint(
        self,
    ) -> None:
        with mock.patch.object(
            agent,
            "dismiss_top_overlay",
            return_value={"ok": True, "dismissed": "task-switcher"},
        ):
            result = agent.handle_method(
                "navigation_action",
                {"action": "back", "input": "swipe"},
            )
        self.assertTrue(result["ok"])
        self.assertEqual(result["schema"], "msys.navigation-action.v1")
        self.assertEqual(result["action"], "back")
        self.assertEqual(result["input"], "swipe")

    @mock.patch.object(agent.MsysClient, "public_call")
    def test_navigation_apps_shows_replaceable_task_switcher(
        self, call: mock.Mock
    ) -> None:
        call.return_value = {
            "type": "return",
            "payload": {"ok": True, "visible": True, "count": 2},
        }
        result = agent.handle_method(
            "navigation_action",
            {"action": "apps", "input": "button"},
        )
        call.assert_called_once_with("role:task-switcher", "show", {}, timeout=7)
        self.assertTrue(result["ok"])
        self.assertEqual(result["action"], "recents")

    @mock.patch.object(agent.MsysClient, "public_call")
    @mock.patch.object(agent, "list_windows")
    def test_back_dismisses_chooser_before_foreground_app(self, list_windows: mock.Mock, call: mock.Mock) -> None:
        list_windows.return_value = [agent.XWindow(
            "0x1",
            "Localized chooser",
            window_id="msys.x11-window.v1:a:0x1",
            identity="org.msys.shell.intent-chooser",
            role="chooser",
            kind="overlay",
        )]
        call.return_value = {"type": "return", "payload": {"cancelled": True}}
        result = agent.handle_method("close_active", {})
        self.assertEqual(result["dismissed"], "chooser")
        call.assert_called_once_with("role:chooser", "cancel_choice", {}, timeout=2)
        list_windows.assert_called_once_with()

    @mock.patch.object(agent.MsysClient, "public_call")
    @mock.patch.object(agent, "list_windows")
    def test_back_hides_notification_center_before_foreground_app(
        self, list_windows: mock.Mock, call: mock.Mock
    ) -> None:
        list_windows.return_value = [agent.XWindow(
            "0x2",
            "Centre de notifications",
            window_id="msys.x11-window.v1:a:0x2",
            identity="org.vendor.notifications",
            role="notification-center",
            kind="overlay",
        )]
        call.return_value = {"type": "return", "payload": {"visible": False}}
        result = agent.handle_method("back", {})
        self.assertEqual(result["dismissed"], "notification-center")
        self.assertEqual(call.call_args_list, [
            mock.call("role:notification-center", "hide", {}, timeout=3),
        ])

    @mock.patch.object(agent, "run")
    def test_get_layout_returns_typed_effective_state(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess(
            [],
            0,
            "msys.layout.effective.v1;profile=mobile;orientation_policy=auto;"
            "insets_policy=auto;orientation=portrait;"
            "screen=360,800;insets=45,0,45,0;workarea=0,45,360,710;"
            "navigation=bottom\n",
            "",
        )
        result = agent.handle_method("get_layout", {})
        self.assertTrue(result["ok"])
        self.assertEqual(result["screen"], {"width": 360, "height": 800})
        self.assertEqual(run.call_args.args[0][-1], "--print-layout")

    @mock.patch.object(agent, "run")
    def test_set_layout_uses_atomic_native_contract(self, run: mock.Mock) -> None:
        run.side_effect = [
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess(
                [],
                0,
                "msys.layout.effective.v1;profile=desktop;orientation_policy=auto;"
                "insets_policy=auto;orientation=landscape;"
                "screen=1280,720;insets=30,0,30,0;workarea=0,30,1280,660;"
                "navigation=bottom\n",
                "",
            ),
        ]
        result = agent.handle_method(
            "set_layout",
            {"profile": "desktop", "orientation": "auto", "insets": "auto"},
        )
        self.assertTrue(result["ok"])
        argv = run.call_args_list[0].args[0]
        self.assertTrue(argv[0].endswith("/bin/msys-x11-policy"))
        self.assertEqual(argv[1:], ["--set-layout", "desktop", "auto", "auto"])
        self.assertEqual(result["requested"], "msys.layout.v1;profile=desktop;orientation=auto;insets=auto")

    @mock.patch.object(agent, "run")
    def test_set_layout_rejects_invalid_payload_before_x11(self, run: mock.Mock) -> None:
        result = agent.handle_method("set_layout", {"profile": "tablet"})
        self.assertFalse(result["ok"])
        self.assertEqual(result["reason"], "invalid-layout")
        run.assert_not_called()


class DisplaySessionLayoutTests(unittest.TestCase):
    def tearDown(self) -> None:
        agent._active_display_session = None
        agent._active_display_session_error = None

    @mock.patch.object(agent, "run")
    def test_spi_portrait_state_is_synced_to_native_policy(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess([], 0, "", "")
        state = display_state(
            provider="org.msys.openstick.ch347:x11-spi-touch-output",
            display=":24",
            width=320,
            height=480,
            matrix=[0, 1, 0, 1, 0, 0, 0, 0, 1],
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "display-session.json"
            write_state(path, state)
            monitor = agent.DisplaySessionMonitor(
                path=path, expected_display=":24", display_switch_grace=0
            )
            self.assertEqual(monitor.poll(force=True), state)
        argv = run.call_args.args[0]
        self.assertEqual(
            argv[-6:],
            ["--sync-display-session", "320", "480", "24", "1", "0,1,0,1,0,0,0,0,1"],
        )

    @mock.patch.object(agent, "run")
    def test_hdmi_landscape_state_syncs_without_inventing_input(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess([], 0, "", "")
        state = display_state(
            provider="org.msys.x11.session:hdmi-output",
            display=":0",
            width=1920,
            height=1080,
            matrix=None,
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "display-session.json"
            write_state(path, state)
            monitor = agent.DisplaySessionMonitor(path=path, expected_display=":0")
            self.assertEqual(monitor.poll(force=True), state)
        self.assertEqual(
            run.call_args.args[0][-6:],
            ["--sync-display-session", "1920", "1080", "24", "0", "none"],
        )

    def test_navigation_region_is_projected_back_to_spi_touch_space(self) -> None:
        layout = {
            "screen": {"width": 320, "height": 480},
            "navigation_region": {"x": 0, "y": 432, "width": 320, "height": 48},
        }
        state = display_state(
            provider="org.msys.openstick.ch347:x11-spi-touch-output",
            display=":24",
            width=320,
            height=480,
            matrix=[0, 1, 0, 1, 0, 0, 0, 0, 1],
        )
        projected = agent.navigation_input_region(layout, state)
        self.assertEqual(
            projected["bounds"],
            {"left": 0.9, "top": 0.0, "right": 1.0, "bottom": 1.0},
        )

    def test_hdmi_without_owned_input_has_no_input_navigation_region(self) -> None:
        layout = {
            "screen": {"width": 1920, "height": 1080},
            "navigation_region": {"x": 1860, "y": 60, "width": 60, "height": 1020},
        }
        state = display_state(
            provider="org.msys.x11.session:hdmi-output",
            display=":0",
            width=1920,
            height=1080,
            matrix=None,
        )
        self.assertIsNone(agent.navigation_input_region(layout, state))

    def test_get_display_session_exposes_the_validated_active_document(self) -> None:
        state = display_state(
            provider="org.msys.x11.session:hdmi-output",
            display=":0",
            width=1920,
            height=1080,
            matrix=None,
        )
        agent._active_display_session = state
        result = agent.handle_method("get_display_session", {})
        self.assertTrue(result["ok"])
        self.assertEqual(result["display_session"]["provider"], state["provider"])

    def test_monitor_requests_restart_when_selected_display_changes(self) -> None:
        state = display_state(
            provider="org.msys.x11.session:hdmi-output",
            display=":0",
            width=1280,
            height=720,
            matrix=None,
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "display-session.json"
            write_state(path, state)
            monitor = agent.DisplaySessionMonitor(
                path=path, expected_display=":24", display_switch_grace=0
            )
            with self.assertRaisesRegex(agent.DisplaySessionChanged, "moved"):
                monitor.poll(force=True)

    @mock.patch.object(agent, "sync_display_session")
    def test_monitor_gives_role_switch_a_bounded_commit_grace(
        self, sync: mock.Mock
    ) -> None:
        state = display_state(
            provider="org.msys.x11.session:hdmi-output",
            display=":0",
            width=1280,
            height=720,
            matrix=None,
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "display-session.json"
            write_state(path, state)
            monitor = agent.DisplaySessionMonitor(
                path=path, expected_display=":24", display_switch_grace=5
            )
            self.assertIsNone(monitor.poll(force=True))
        sync.assert_not_called()

    def test_successful_layout_readback_broadcasts_typed_change(self) -> None:
        client = mock.Mock(spec=agent.MsysClient)
        state = display_state(
            provider="org.msys.openstick.ch347:x11-spi-touch-output",
            display=":24",
            width=800,
            height=480,
            matrix=[1, 0, 0, 0, 1, 0, 0, 0, 1],
        )
        state["generation"] = 7
        layout = {
            "ok": True,
            "profile": "mobile",
            "orientation_policy": "auto",
            "orientation": "landscape",
            "screen": {"width": 800, "height": 480},
            "insets": {"top": 60, "right": 60, "bottom": 0, "left": 0},
            "workarea": {"x": 0, "y": 60, "width": 740, "height": 420},
            "navigation_edge": "right",
            "navigation_region": {"x": 740, "y": 60, "width": 60, "height": 420},
            "display_session": state,
            "display_consistent": True,
        }
        self.assertTrue(agent.emit_method_events(client, "set_layout", layout))
        topic, payload = client.event.call_args.args
        self.assertEqual(topic, "msys.layout.changed")
        self.assertEqual(payload["profile"], "mobile")
        self.assertEqual(payload["orientation"], "landscape")
        self.assertEqual(payload["geometry"], {"width": 800, "height": 480})
        self.assertEqual(payload["display_generation"], 7)
        self.assertEqual(payload["display_provider"], state["provider"])

    def test_failed_layout_update_does_not_broadcast(self) -> None:
        client = mock.Mock(spec=agent.MsysClient)
        self.assertFalse(
            agent.emit_method_events(client, "set_layout", {"ok": False})
        )
        self.assertFalse(agent.emit_method_events(client, "get_layout", {"ok": True}))
        client.event.assert_not_called()


if __name__ == "__main__":
    unittest.main()
