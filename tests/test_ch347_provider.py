from __future__ import annotations

import os
import json
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "msys_ch347_x11_provider.sh"


class Ch347ProviderOwnershipTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.run_dir = self.root / "run"
        self.bin_dir = self.root / "bin"
        self.bin_dir.mkdir()
        self.start = self.root / "start.sh"
        self.stop = self.root / "stop.sh"
        self.start.write_text(
            "#!/bin/sh\n"
            "mkdir -p \"$RUN_DIR\"\n"
            "sleep 60 &\n"
            "echo $! > \"$RUN_DIR/pids\"\n",
            encoding="utf-8",
        )
        self.stop.write_text(
            "#!/bin/sh\n"
            "echo stop >> \"$RUN_DIR/stop.log\"\n"
            "if [ -f \"$RUN_DIR/pids\" ]; then\n"
            "  while read pid; do kill \"$pid\" 2>/dev/null || true; done < \"$RUN_DIR/pids\"\n"
            "fi\n"
            "rm -f \"$RUN_DIR/pids\"\n",
            encoding="utf-8",
        )
        xdpyinfo = self.bin_dir / "xdpyinfo"
        xdpyinfo.write_text(
            "#!/bin/sh\n"
            "test -s \"$RUN_DIR/pids\" || exit 1\n"
            "pid=$(sed -n '1p' \"$RUN_DIR/pids\")\n"
            "kill -0 \"$pid\" 2>/dev/null || exit 1\n"
            "echo '  dimensions:    320x480 pixels (85x127 millimeters)'\n"
            "echo '  depth of root window:    24 planes'\n"
            "echo '    XTEST'\n",
            encoding="utf-8",
        )
        for path in (self.start, self.stop, xdpyinfo):
            path.chmod(0o755)
        self.processes: list[subprocess.Popen[str]] = []

    def tearDown(self) -> None:
        for process in reversed(self.processes):
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            if process.stdout is not None:
                process.stdout.close()
        self.temporary.cleanup()

    def spawn(
        self, generation: int, extra_env: dict[str, str] | None = None
    ) -> subprocess.Popen[str]:
        env = os.environ.copy()
        for name in (
            "CH347_TOUCH_X_MIN",
            "CH347_TOUCH_SWAP_XY",
            "CH347_TOUCH_INVERT_X",
            "CH347_TOUCH_INVERT_Y",
        ):
            env.pop(name, None)
        env.update(
            {
                "PATH": f"{self.bin_dir}:{env['PATH']}",
                "RUN_DIR": str(self.run_dir),
                "CH347_START_SCRIPT": str(self.start),
                "CH347_STOP_SCRIPT": str(self.stop),
                "MSYS_X11_READY_FILE": str(self.run_dir / "msys.ready"),
                "MSYS_RUNTIME_DIR": str(self.run_dir / "broker"),
                "MSYS_GENERATION": str(generation),
                "DISPLAY": ":24",
                "CH347_TOUCH": "1",
            }
        )
        env.update(extra_env or {})
        process = subprocess.Popen(
            ["bash", str(SCRIPT)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
        self.processes.append(process)
        return process

    def wait_ready(self, timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (self.run_dir / "msys.ready").exists():
                return
            time.sleep(0.05)
        self.fail("provider did not publish ready file")

    def stop_count(self) -> int:
        path = self.run_dir / "stop.log"
        return len(path.read_text(encoding="utf-8").splitlines()) if path.exists() else 0

    @staticmethod
    def finished_output(process: subprocess.Popen[str]) -> str:
        if process.poll() is None or process.stdout is None:
            return "<still running>"
        return process.stdout.read()

    def test_old_generation_exit_does_not_stop_replacement_stack(self) -> None:
        old = self.spawn(1)
        self.wait_ready()
        state = json.loads((self.run_dir / "msys.ready").read_text(encoding="utf-8"))
        self.assertEqual(state["schema"], "msys.display-session.v1")
        self.assertEqual(state["display"], ":24")
        self.assertEqual(state["geometry"], {"width": 320, "height": 480, "depth": 24})
        self.assertEqual(state["input_transform"]["source"], "ch347-direct-effective")
        active_state = self.run_dir / "broker" / "display-session.json"
        self.assertTrue(active_state.exists())
        self.assertEqual(json.loads(active_state.read_text(encoding="utf-8"))["generation"], 1)
        old_pid = (self.run_dir / "pids").read_text(encoding="utf-8").strip()

        replacement = self.spawn(2)
        deadline = time.monotonic() + 7
        while time.monotonic() < deadline:
            ready = self.run_dir / "msys.ready"
            pid_file = self.run_dir / "pids"
            if ready.exists() and pid_file.exists():
                new_pid = pid_file.read_text(encoding="utf-8").strip()
                if new_pid and new_pid != old_pid:
                    break
            time.sleep(0.05)
        else:
            self.fail("replacement provider did not own a new child")

        self.assertEqual(
            self.stop_count(),
            1,
            "replacement should stop the old stack once; "
            f"old_rc={old.poll()} old_output={self.finished_output(old)!r}; "
            f"replacement_rc={replacement.poll()} "
            f"replacement_output={self.finished_output(replacement)!r}",
        )
        old.send_signal(signal.SIGTERM)
        self.assertEqual(old.wait(timeout=5), 0)
        time.sleep(0.2)

        self.assertIsNone(replacement.poll())
        self.assertTrue((self.run_dir / "pids").exists())
        self.assertEqual(self.stop_count(), 1, "old EXIT trap killed the replacement")
        self.assertEqual(
            json.loads(active_state.read_text(encoding="utf-8"))["generation"],
            2,
            "superseded provider must not republish stale display-session state",
        )

        replacement.send_signal(signal.SIGTERM)
        self.assertEqual(replacement.wait(timeout=5), 0)
        self.assertEqual(self.stop_count(), 2)
        self.assertFalse(active_state.exists())

    def test_ready_file_handoff_cannot_fail_publisher_generation(self) -> None:
        # Hold generation 1 inside its successful READY publisher until the
        # replacement has removed the shared ready file.  This deterministically
        # models the slow-aarch64 window where the old implementation broke out
        # of its probe loop, observed the replacement's unlink, and exited 1.
        slow_python = self.root / "slow-python.sh"
        slow_python.write_text(
            "#!/bin/sh\n"
            '"$REAL_PYTHON" "$@"\n'
            "rc=$?\n"
            'if [ "$rc" -eq 0 ] && [ "${WAIT_FOR_READY_REMOVAL:-0}" = 1 ] && '
            '[ "${MSYS_GENERATION:-}" = 1 ]; then\n'
            '  last=""; for value in "$@"; do last=$value; done\n'
            '  if [ "$last" = "$MSYS_X11_READY_FILE" ]; then\n'
            "    for _ in $(seq 1 500); do\n"
            '      [ ! -e "$MSYS_X11_READY_FILE" ] && break\n'
            "      sleep 0.01\n"
            "    done\n"
            "  fi\n"
            "fi\n"
            'exit "$rc"\n',
            encoding="utf-8",
        )
        slow_python.chmod(0o755)

        old = self.spawn(1, {
            "MSYS_PYTHON": str(slow_python),
            "REAL_PYTHON": sys.executable,
            "WAIT_FOR_READY_REMOVAL": "1",
        })
        self.wait_ready()
        old_pid = (self.run_dir / "pids").read_text(encoding="utf-8").strip()

        replacement = self.spawn(2)
        deadline = time.monotonic() + 7
        while time.monotonic() < deadline:
            pid_file = self.run_dir / "pids"
            if pid_file.exists():
                new_pid = pid_file.read_text(encoding="utf-8").strip()
                if new_pid and new_pid != old_pid:
                    break
            time.sleep(0.05)
        else:
            self.fail("replacement provider did not own a new child")

        self.assertEqual(
            old.wait(timeout=5),
            0,
            f"superseded publisher failed: {self.finished_output(old)!r}",
        )
        self.assertIsNone(replacement.poll())
        self.assertEqual(self.stop_count(), 1)

        active_state = self.run_dir / "broker" / "display-session.json"
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if active_state.exists():
                state = json.loads(active_state.read_text(encoding="utf-8"))
                if state.get("generation") == 2:
                    break
            time.sleep(0.05)
        else:
            self.fail("replacement did not publish generation 2 state")

        replacement.send_signal(signal.SIGTERM)
        self.assertEqual(replacement.wait(timeout=5), 0)

    def test_opt_in_fallback_switches_to_xtest_when_native_device_is_missing(self) -> None:
        xinput = self.bin_dir / "xinput"
        xinput.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        xinput.chmod(0o755)
        process = self.spawn(
            9,
            {
                "MSYS_CH347_XTEST_FALLBACK": "1",
                "MSYS_CH347_NATIVE_TOUCH_PROBES": "1",
            },
        )
        self.wait_ready()
        self.assertEqual(
            (self.run_dir / "touch_mode").read_text(encoding="utf-8").strip(),
            "mouse",
        )
        state = json.loads((self.run_dir / "msys.ready").read_text(encoding="utf-8"))
        self.assertEqual(state["input_transform"]["mode"], "ch347-xtest")
        self.assertEqual(
            state["input_transform"]["source"], "ch347-xtest-effective"
        )
        process.send_signal(signal.SIGTERM)
        self.assertEqual(process.wait(timeout=5), 0)

    def test_published_transform_uses_the_same_calibration_as_the_sink(self) -> None:
        calibration = self.root / "touch.env"
        calibration.write_text(
            "CH347_TOUCH=1\n"
            "CH347_TOUCH_SWAP_XY=1\n"
            "CH347_TOUCH_INVERT_X=0\n"
            "CH347_TOUCH_INVERT_Y=1\n"
            "CH347_TOUCH_X_MIN=200\n",
            encoding="utf-8",
        )
        process = self.spawn(10, {"CH347_TOUCH_CAL_FILE": str(calibration)})
        self.wait_ready()
        state = json.loads((self.run_dir / "msys.ready").read_text(encoding="utf-8"))
        self.assertEqual(
            state["input_transform"]["matrix"],
            [0, 1, 0, -1, 0, 1, 0, 0, 1],
        )
        process.send_signal(signal.SIGTERM)
        self.assertEqual(process.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
