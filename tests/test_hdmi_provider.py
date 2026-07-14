from __future__ import annotations

import json
import os
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "msys_hdmi_x11_provider.sh"


class HdmiProviderTests(unittest.TestCase):
    def test_provider_publishes_live_state_then_owns_xorg_until_shutdown(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = root / "run"
            bin_dir = root / "bin"
            bin_dir.mkdir()
            xorg = bin_dir / "Xorg"
            xdpyinfo = bin_dir / "xdpyinfo"
            xorg.write_text(
                "#!/bin/sh\n"
                "mkdir -p \"$MSYS_HDMI_RUN_DIR\"\n"
                "echo $$ > \"$MSYS_HDMI_RUN_DIR/xorg.pid\"\n"
                "exec sleep 60\n",
                encoding="utf-8",
            )
            xdpyinfo.write_text(
                "#!/bin/sh\n"
                "test \"$DISPLAY\" = ':91' || exit 2\n"
                "test -s \"$MSYS_HDMI_RUN_DIR/xorg.pid\" || exit 1\n"
                "pid=$(sed -n '1p' \"$MSYS_HDMI_RUN_DIR/xorg.pid\")\n"
                "kill -0 \"$pid\" 2>/dev/null || exit 1\n"
                "echo '  dimensions:    1280x720 pixels (300x170 millimeters)'\n"
                "echo '  depth of root window:    24 planes'\n",
                encoding="utf-8",
            )
            for executable in (xorg, xdpyinfo):
                executable.chmod(0o755)
            ready_file = run_dir / "ready.json"
            runtime_dir = run_dir / "broker"
            state_file = runtime_dir / "display-session.json"
            env = os.environ.copy()
            env.update({
                "PATH": f"{bin_dir}:{env['PATH']}",
                "DISPLAY": ":91",
                "DISPLAY_ID": ":91",
                "MSYS_COMPONENT_ID": "org.msys.x11.session:hdmi-output",
                "MSYS_GENERATION": "5",
                "MSYS_HDMI_RUN_DIR": str(run_dir),
                "MSYS_RUNTIME_DIR": str(runtime_dir),
                "MSYS_XORG_BINARY": str(xorg),
                "MSYS_X11_READY_FILE": str(ready_file),
                "MSYS_DISPLAY_INPUT_MODE": "none",
            })
            process = subprocess.Popen(
                ["bash", str(SCRIPT)],
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                deadline = time.monotonic() + 6
                while time.monotonic() < deadline and not (
                    state_file.exists() and ready_file.exists()
                ):
                    if process.poll() is not None:
                        output = process.stdout.read() if process.stdout else ""
                        self.fail(f"HDMI provider exited before ready rc={process.returncode}: {output}")
                    time.sleep(0.05)
                self.assertTrue(state_file.exists(), "HDMI provider did not publish state")
                self.assertTrue(ready_file.exists(), "HDMI provider did not publish readiness")
                state = json.loads(state_file.read_text(encoding="utf-8"))
                self.assertEqual(state["schema"], "msys.display-session.v1")
                self.assertEqual(state["state"], "ready")
                self.assertEqual(state["display"], ":91")
                self.assertEqual(state["generation"], 5)
                self.assertEqual(
                    state["geometry"], {"width": 1280, "height": 720, "depth": 24}
                )
                self.assertFalse(state["input_transform"]["enabled"])
                self.assertIsNone(process.poll())

                xorg_pid = int((run_dir / "xorg.pid").read_text(encoding="utf-8"))
                process.send_signal(signal.SIGTERM)
                self.assertEqual(process.wait(timeout=5), 0)
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    try:
                        os.kill(xorg_pid, 0)
                    except ProcessLookupError:
                        break
                    time.sleep(0.05)
                else:
                    self.fail("HDMI wrapper left its Xorg process running")
                self.assertFalse(state_file.exists())
                self.assertFalse(ready_file.exists())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                if process.stdout is not None:
                    process.stdout.close()


if __name__ == "__main__":
    unittest.main()
