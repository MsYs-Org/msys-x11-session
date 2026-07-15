from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from msys_x11_session import window_policy_agent as agent


ROOT = Path(__file__).resolve().parents[1]
WINDOW_MANAGER_CONTRACT = {
    "id": "org.msys.role.window-manager.v1",
    "version": "1.0.0",
}
DISPLAY_OUTPUT_CONTRACT = {
    "id": "org.msys.role.display-output.v1",
    "version": "1.0.0",
}


def role_provide(component: dict[str, object], role: str) -> dict[str, object]:
    return next(item for item in component["provides"] if item.get("role") == role)


class CanonicalManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8-sig"))
        cls.components = {
            component["id"]: component for component in cls.manifest["components"]
        }

    def test_window_policy_belongs_to_independent_x11_session_package(self) -> None:
        self.assertEqual(self.manifest["schema"], "msys.manifest.v1")
        self.assertEqual(self.manifest["package"]["id"], "org.msys.x11.session")
        policy = self.components["window-policy"]
        self.assertEqual(policy["runtime"], "c")
        self.assertEqual(policy["exec"], ["@package/bin/msys-x11-policy"])
        fallback = self.components["window-policy-python"]
        self.assertEqual(fallback["lifecycle"], "on-demand")
        self.assertEqual(fallback["idle_timeout_ms"], 30000)
        self.assertLess(
            role_provide(fallback, "window-policy")["priority"],
            role_provide(policy, "window-policy")["priority"],
        )
        self.assertNotIn(
            "window-manager",
            {item.get("role") for item in fallback["provides"]},
        )
        self.assertNotIn("PYTHONPATH", policy.get("env", {}))
        self.assertNotIn("DISPLAY", policy.get("env", {}))
        self.assertEqual(policy["windowing"]["display"], "inherit")
        self.assertIn(
            "mipc.event:publish:msys.layout.changed", policy["permissions"]
        )
        roles = {item.get("role") for item in policy["provides"] if "role" in item}
        self.assertEqual(roles, {"window-policy", "window-manager"})
        self.assertEqual(
            role_provide(policy, "window-manager")["x-msys-contract"],
            WINDOW_MANAGER_CONTRACT,
        )
        interfaces = {
            item.get("interface")
            for item in policy["provides"]
            if "interface" in item
        }
        self.assertIn("org.msys.window-manager.v1", interfaces)
        self.assertIn("mipc.call:role:screen-shield", policy["permissions"])
        self.assertEqual(policy["readiness"]["mode"], "mipc-ready")
        self.assertIn("src/msys_x11_agent.c", {
            str(path.relative_to(ROOT)).replace("\\", "/")
            for path in (ROOT / "src").glob("*.c")
        })

    def test_all_canonical_package_exec_paths_exist(self) -> None:
        for component in self.manifest["components"]:
            for argument in component["exec"]:
                if argument.startswith("@package/"):
                    with self.subTest(component=component["id"], path=argument):
                        target = ROOT / argument.removeprefix("@package/")
                        if argument == "@package/bin/msys-x11-policy":
                            self.assertTrue((ROOT / "src" / "msys_x11_policy.c").is_file())
                            self.assertIn(
                                "bin/msys-x11-policy",
                                (ROOT / "Makefile").read_text(encoding="utf-8"),
                            )
                        else:
                            self.assertTrue(target.is_file())

    def test_native_debug_gesture_cli_is_packaged_without_xdotool(self) -> None:
        source = (ROOT / "src" / "msys_x11_policy.c").read_text(encoding="utf-8")
        self.assertIn("--debug-swipe-identity", source)
        self.assertIn("--debug-swipe-window", source)
        self.assertIn("XTestFakeMotionEvent", source)
        self.assertIn("XTestFakeButtonEvent", source)
        self.assertNotIn('system("xdotool', source)
        self.assertNotIn('execlp("xdotool', source)
        self.assertIn("-ldl", (ROOT / "Makefile").read_text(encoding="utf-8"))

    def test_home_snapshot_precedes_launcher_and_obscured_tasks_keep_cache(self) -> None:
        agent = (ROOT / "src" / "msys_x11_agent.c").read_text(encoding="utf-8")
        policy = (ROOT / "src" / "msys_x11_policy.c").read_text(encoding="utf-8")
        home = agent[agent.index("static char *home_action"):agent.index(
            "static const char *overlay_method"
        )]
        self.assertLess(
            home.index("msys_x11_policy_list_windows_json"),
            home.index('"activate_role"'),
        )
        self.assertIn("int task_surface_seen = 0;", policy)
        self.assertIn(
            "task_switcher_visible, task_surface_seen, thumbnail,", policy
        )
        self.assertIn("!task_surface_above", policy)

    def test_window_manager_applies_redirected_activation_stacking(self) -> None:
        policy = (ROOT / "src" / "msys_x11_policy.c").read_text(encoding="utf-8")
        configure = policy[
            policy.index("case ConfigureRequest:"):
            policy.index("case MapNotify:")
        ]
        self.assertIn("request->value_mask & CWStackMode", configure)
        self.assertIn("XConfigureWindow(display, request->window", configure)
        self.assertLess(
            configure.index("XConfigureWindow(display, request->window"),
            configure.index("raise_system_overlays(display, root)"),
        )

    def test_back_and_close_active_have_distinct_application_semantics(self) -> None:
        agent = (ROOT / "src" / "msys_x11_agent.c").read_text(encoding="utf-8")
        self.assertIn('"navigation_back", "{}", 1500', agent)
        self.assertIn('"background_component"', agent)
        self.assertIn("msys_x11_policy_component_window", agent)
        self.assertIn("msys_x11_agent_home_visible(component, window.role)", agent)
        self.assertIn('agent->display, "minimize"', agent)
        self.assertIn("backgrounded_component", agent)
        self.assertIn('return back_action(agent, 1);', agent)
        self.assertIn('return back_action(agent, 0);', agent)
        self.assertIn('delegated = back_action(agent, 1);', agent)

    def test_ch347_wrapper_prefers_a_package_owned_x11display_tree(self) -> None:
        source = (ROOT / "scripts" / "msys_ch347_x11_provider.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("MSYS_PACKAGE_ROOT/files/x11display", source)
        self.assertIn("MSYS_APP_STATE_DIR", source)
        self.assertIn("$SESSION_ROOT/files/x11display", source)
        self.assertIn(
            "$X11DISPLAY_ROOT/scripts/start_ch347_dirty_usb_x11.sh", source
        )
        self.assertIn("$X11DISPLAY_ROOT/ch347/touch_calibration.env", source)
        self.assertIn("X11DISPLAY_ROOT=/root/x11display", source)
        self.assertIn("--publish-display-session", source)

    def test_hdmi_readiness_waits_for_a_real_x11_display(self) -> None:
        canonical = self.components["hdmi-output"]
        legacy_document = json.loads(
            (ROOT / "manifests" / "x11-session-hdmi-template.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            legacy_document["package"]["x-msys-template"],
            "promote-to-package-root-before-use",
        )
        legacy = legacy_document["components"][0]
        for component in (canonical, legacy):
            with self.subTest(package=component["exec"]):
                self.assertEqual(component["readiness"]["mode"], "x11-display")
                self.assertEqual(component["env"]["DISPLAY_ID"], ":0")
                self.assertIn("MSYS_X11_READY_FILE", component["env"])
                self.assertNotIn("MSYS_DISPLAY_SESSION_STATE_FILE", component["env"])
                capabilities = {
                    item.get("capability") for item in component["provides"]
                }
                self.assertIn("display.session.v1", capabilities)
                self.assertEqual(
                    role_provide(component, "display-output")["x-msys-contract"],
                    DISPLAY_OUTPUT_CONTRACT,
                )

    def test_ch347_identity_and_real_display_readiness_remain_compatible(self) -> None:
        component = json.loads(
            (ROOT / "manifests" / "openstick-ch347-x11.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            component["package"]["x-msys-template"],
            "promote-to-package-root-before-use",
        )
        component = component["components"][0]
        self.assertEqual(component["id"], "x11-spi-touch-output")
        self.assertEqual(component["readiness"]["mode"], "x11-display")
        self.assertEqual(component["env"]["DISPLAY_ID"], ":24")
        self.assertEqual(component["env"]["MSYS_DISPLAY_INPUT_MODE"], "ch347-direct")
        self.assertEqual(
            component["exec"],
            ["bash", "@package/scripts/msys_ch347_x11_provider.sh"],
        )
        self.assertEqual(
            role_provide(component, "display-output")["x-msys-contract"],
            DISPLAY_OUTPUT_CONTRACT,
        )

    def test_compatibility_spi_manifest_claims_display_output_contract(self) -> None:
        component = json.loads(
            (ROOT / "manifests" / "x11display-spi.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            component["package"]["x-msys-template"],
            "promote-to-package-root-before-use",
        )
        component = component["components"][0]
        self.assertEqual(
            role_provide(component, "display-output")["x-msys-contract"],
            DISPLAY_OUTPUT_CONTRACT,
        )

    def test_nested_compatibility_templates_resolve_in_source_tree(self) -> None:
        for name in (
            "openstick-ch347-x11.json",
            "x11display-spi.json",
            "x11-session-hdmi-template.json",
        ):
            path = ROOT / "manifests" / name
            document = json.loads(path.read_text(encoding="utf-8"))
            argument = document["components"][0]["exec"][1]
            self.assertTrue(argument.startswith("@package/"))
            resolved = path.parent / argument.removeprefix("@package/")
            with self.subTest(name=name, resolved=resolved):
                self.assertTrue(resolved.is_file())


class IsolatedPackageRuntimeTests(unittest.TestCase):
    def _run_entry(self, root: Path) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["PYTHONPATH"] = "/path/that/must/not/be-used"
        return subprocess.run(
            [sys.executable, "-I", str(root / "scripts" / "msys_window_policy_entry.py"), "--check-import"],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )

    def test_source_tree_entry_imports_without_external_pythonpath(self) -> None:
        result = self._run_entry(ROOT)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("msys-window-policy-entry: ok", result.stdout)

    def test_installed_package_root_imports_without_external_pythonpath(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            installed = Path(temporary)
            shutil.copytree(ROOT / "msys_x11_session", installed / "msys_x11_session")
            (installed / "scripts").mkdir()
            shutil.copy2(
                ROOT / "scripts" / "msys_window_policy_entry.py",
                installed / "scripts" / "msys_window_policy_entry.py",
            )
            result = self._run_entry(installed)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"root={installed}", result.stdout)

    def test_missing_native_binary_fails_without_make_or_compiler(self) -> None:
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(
            os.environ,
            {
                "MSYS_PACKAGE_ROOT": temporary,
                "MSYS_X11_SESSION_ROOT": "",
                "MSYS_X11_POLICY_BINARY": "",
            },
            clear=False,
        ), mock.patch.object(agent.subprocess, "Popen") as popen, mock.patch.object(
            agent.subprocess, "run"
        ) as run:
            with self.assertRaisesRegex(FileNotFoundError, "make all"):
                agent.start_native_policy()
            popen.assert_not_called()
            run.assert_not_called()

    def test_explicit_package_root_does_not_borrow_source_binary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            package_root = base / "installed-package"
            package_root.mkdir()
            source_module = base / "source" / "msys_x11_session" / "window_policy_agent.py"
            source_binary = base / "source" / "bin" / "msys-x11-policy"
            source_module.parent.mkdir(parents=True)
            source_module.touch()
            source_binary.parent.mkdir(parents=True)
            source_binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            source_binary.chmod(source_binary.stat().st_mode | stat.S_IXUSR)

            expected = package_root / "bin" / "msys-x11-policy"
            with mock.patch.dict(
                os.environ,
                {
                    "MSYS_PACKAGE_ROOT": str(package_root),
                    "MSYS_X11_SESSION_ROOT": "",
                    "MSYS_X11_POLICY_BINARY": "",
                },
                clear=False,
            ), mock.patch.object(agent, "__file__", str(source_module)), mock.patch.object(
                agent.subprocess, "Popen"
            ) as popen:
                self.assertEqual(agent.native_policy_binary(), str(expected))
                with self.assertRaisesRegex(FileNotFoundError, str(expected).replace("\\", "\\\\")):
                    agent.start_native_policy()
            popen.assert_not_called()

    def test_prebuilt_package_binary_is_started_directly(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "bin" / "msys-x11-policy"
            binary.parent.mkdir()
            binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
            sentinel = object()
            with mock.patch.dict(
                os.environ,
                {
                    "MSYS_PACKAGE_ROOT": str(root),
                    "MSYS_X11_SESSION_ROOT": "",
                    "MSYS_X11_POLICY_BINARY": "",
                    "DISPLAY_ID": ":81",
                },
                clear=False,
            ), mock.patch.object(agent.subprocess, "Popen", return_value=sentinel) as popen:
                self.assertIs(agent.start_native_policy(), sentinel)
            self.assertEqual(popen.call_args.args[0], [str(binary)])
            self.assertEqual(popen.call_args.kwargs["env"]["DISPLAY"], os.environ.get("DISPLAY", ":81"))
            self.assertEqual(
                popen.call_args.kwargs["env"]["MSYS_X11_NATIVE_AGENT"], "0"
            )


if __name__ == "__main__":
    unittest.main()
