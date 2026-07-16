from __future__ import annotations

import json
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any

from msys_x11_session.display_session import (
    DisplaySessionError,
    SCHEMA,
    ch347_transform,
    effective_input_transform,
    load_state,
    observe_display_session,
    parse_matrix,
    parse_xdpyinfo,
    parse_xinput_transform,
    validate_state,
    write_state,
)


XDPYINFO = """
name of display:    :24
screen #0:
  dimensions:    320x480 pixels (85x127 millimeters)
  depth of root window:    24 planes
"""


class DisplayProbeTests(unittest.TestCase):
    def test_xdpyinfo_parser_uses_live_root_geometry(self) -> None:
        self.assertEqual(
            parse_xdpyinfo(XDPYINFO),
            {"width": 320, "height": 480, "depth": 24},
        )
        with self.assertRaisesRegex(DisplaySessionError, "dimensions"):
            parse_xdpyinfo("name of display: :24")

    def test_ch347_transform_matches_swap_and_invert_flags(self) -> None:
        self.assertEqual(
            ch347_transform({}),
            (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
        )
        self.assertEqual(
            ch347_transform({
                "CH347_TOUCH_SWAP_XY": "1",
                "CH347_TOUCH_INVERT_X": "1",
            }),
            (0.0, -1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0),
        )
        self.assertEqual(
            ch347_transform({"CH347_DISPLAY_ROTATION": "right"}),
            (0.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 1.0),
        )
        with self.assertRaisesRegex(DisplaySessionError, "rotation"):
            ch347_transform({"CH347_DISPLAY_ROTATION": "diagonal"})

    def test_xinput_matrix_is_reported_as_observed_not_assumed(self) -> None:
        text = (
            "Coordinate Transformation Matrix (123):\t"
            "0.000000, -1.000000, 1.000000, 1.000000, 0.000000, 0.000000, 0, 0, 1\n"
        )
        self.assertEqual(
            parse_xinput_transform(text),
            (0.0, -1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0),
        )

        calls: list[list[str]] = []

        def runner(argv: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, text, "")

        transform = effective_input_transform({
            "DISPLAY": ":0",
            "MSYS_DISPLAY_INPUT_MODE": "xinput",
            "MSYS_INPUT_DEVICE": "Touchscreen",
        }, runner=runner)
        self.assertEqual(calls, [["xinput", "list-props", "Touchscreen"]])
        self.assertTrue(transform["verified"])
        self.assertEqual(transform["source"], "xinput-coordinate-transformation-matrix")
        self.assertEqual(transform["matrix"], [0, -1, 1, 1, 0, 0, 0, 0, 1])

    def test_observation_contains_actual_display_geometry_and_direct_input(self) -> None:
        calls: list[tuple[list[str], str]] = []

        def runner(argv: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
            calls.append((argv, kwargs["env"]["DISPLAY"]))
            return subprocess.CompletedProcess(argv, 0, XDPYINFO, "")

        state = observe_display_session(
            display=":24",
            provider="org.msys.openstick.ch347:x11-spi-touch-output",
            env={
                "MSYS_GENERATION": "7",
                "CH347_TOUCH": "1",
                "CH347_TOUCH_INVERT_Y": "1",
                "MSYS_DISPLAY_INPUT_MODE": "ch347-direct",
            },
            runner=runner,
        )
        self.assertEqual(calls, [(["xdpyinfo"], ":24")])
        self.assertEqual(state["schema"], SCHEMA)
        self.assertEqual(state["state"], "ready")
        self.assertEqual(state["generation"], 7)
        self.assertEqual(state["display"], ":24")
        self.assertEqual(state["geometry"], {"width": 320, "height": 480, "depth": 24})
        self.assertEqual(state["input_transform"]["matrix"], [1, 0, 0, 0, -1, 1, 0, 0, 1])
        self.assertIs(validate_state(state), state)

    def test_hdmi_without_owned_input_reports_none_instead_of_guessing(self) -> None:
        transform = effective_input_transform({"MSYS_DISPLAY_INPUT_MODE": "none"})
        self.assertFalse(transform["enabled"])
        self.assertIsNone(transform["matrix"])
        self.assertTrue(transform["verified"])

    def test_optional_ch347_xtest_fallback_preserves_effective_mapping(self) -> None:
        transform = effective_input_transform({
            "MSYS_DISPLAY_INPUT_MODE": "ch347-xtest",
            "CH347_TOUCH": "1",
            "CH347_TOUCH_SWAP_XY": "1",
            "CH347_TOUCH_INVERT_Y": "1",
        })
        self.assertEqual(transform["mode"], "ch347-xtest")
        self.assertEqual(transform["source"], "ch347-xtest-effective")
        self.assertEqual(transform["matrix"], [0, 1, 0, -1, 0, 1, 0, 0, 1])

    def test_matrix_contract_rejects_non_finite_or_wrong_shape(self) -> None:
        for value in (
            "1,0,0",
            "1 0 0 0 1 0 0 0 nan",
            [1] * 8,
            [1, 0, 0, 0, 0, 0, 0, 0, 1],
        ):
            with self.subTest(value=value), self.assertRaises(DisplaySessionError):
                parse_matrix(value)


class DisplayStateFileTests(unittest.TestCase):
    def test_state_is_atomically_persisted_and_strictly_loaded(self) -> None:
        document = {
            "schema": SCHEMA,
            "state": "ready",
            "provider": "org.msys.x11.session:hdmi-output",
            "generation": 2,
            "display": ":0",
            "geometry": {"width": 1920, "height": 1080, "depth": 24},
            "input_transform": {
                "enabled": False,
                "mode": "none",
                "device": None,
                "space": "normalized-display",
                "matrix": None,
                "source": "no-provider-owned-input",
                "verified": True,
            },
            "observed_at_unix_ms": 123,
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "nested" / "display-session.json"
            self.assertEqual(write_state(path, document), path)
            self.assertEqual(load_state(path), document)
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), document)
            self.assertEqual(list(path.parent.glob(f".{path.name}.*")), [])

    def test_timestamp_only_reobservation_preserves_state_file_identity(self) -> None:
        document = {
            "schema": SCHEMA,
            "state": "ready",
            "provider": "org.msys.openstick.ch347:x11-spi-touch-output",
            "generation": 7,
            "display": ":24",
            "geometry": {"width": 320, "height": 480, "depth": 24},
            "input_transform": {
                "enabled": True,
                "mode": "ch347-direct",
                "device": "CH347 XPT2046",
                "space": "normalized-display",
                "matrix": [1, 0, 0, 0, 1, 0, 0, 0, 1],
                "source": "ch347-direct-effective",
                "verified": True,
            },
            "observed_at_unix_ms": 100,
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "display-session.json"
            write_state(path, document)
            original_bytes = path.read_bytes()
            original_stat = path.stat()

            reobserved = dict(document)
            reobserved["observed_at_unix_ms"] = 200
            time.sleep(0.002)
            self.assertEqual(write_state(path, reobserved), path)

            unchanged_stat = path.stat()
            self.assertEqual(path.read_bytes(), original_bytes)
            self.assertEqual(unchanged_stat.st_ino, original_stat.st_ino)
            self.assertEqual(unchanged_stat.st_mtime_ns, original_stat.st_mtime_ns)

            changed = dict(reobserved)
            changed["generation"] = 8
            write_state(path, changed)
            self.assertEqual(load_state(path), changed)
            self.assertNotEqual(path.read_bytes(), original_bytes)

    def test_same_generation_rotation_replaces_geometry_and_input_matrix(self) -> None:
        document = {
            "schema": SCHEMA,
            "state": "ready",
            "provider": "org.msys.openstick.ch347:x11-spi-touch-output",
            "generation": 7,
            "display": ":24",
            "geometry": {"width": 320, "height": 480, "depth": 24},
            "input_transform": {
                "enabled": True,
                "mode": "ch347-direct",
                "device": "CH347 XPT2046",
                "space": "normalized-display",
                "matrix": [1, 0, 0, 0, 1, 0, 0, 0, 1],
                "source": "ch347-direct-effective",
                "verified": True,
            },
            "observed_at_unix_ms": 100,
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "display-session.json"
            write_state(path, document)
            original_inode = path.stat().st_ino
            rotated = dict(document)
            rotated["geometry"] = {"width": 480, "height": 320, "depth": 24}
            rotated["input_transform"] = {
                **document["input_transform"],
                "matrix": [0, 1, 0, -1, 0, 1, 0, 0, 1],
            }
            rotated["observed_at_unix_ms"] = 200

            write_state(path, rotated)

            self.assertEqual(load_state(path), rotated)
            self.assertNotEqual(path.stat().st_ino, original_inode)

    def test_invalid_ready_document_is_rejected(self) -> None:
        with self.assertRaises(DisplaySessionError):
            validate_state({"schema": SCHEMA, "state": "starting"})


if __name__ == "__main__":
    unittest.main()
