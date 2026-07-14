from __future__ import annotations

import json
import unittest
from pathlib import Path

from msys_x11_session.layout_contract import Insets, LayoutConfig, parse_effective


class LayoutContractTests(unittest.TestCase):
    def test_config_round_trip(self) -> None:
        config = LayoutConfig.from_payload(
            {
                "profile": "mobile",
                "orientation": "landscape",
                "insets": {"top": 5, "right": 6, "bottom": 7, "left": 8},
            }
        )
        self.assertEqual(config.insets, Insets(5, 6, 7, 8))
        self.assertEqual(
            config.encode(),
            "msys.layout.v1;profile=mobile;orientation=landscape;insets=5,6,7,8",
        )
        self.assertEqual(LayoutConfig.decode(config.encode()), config)

    def test_legacy_mode_alias_is_unambiguous(self) -> None:
        self.assertEqual(LayoutConfig.from_payload({"mode": "kiosk"}).profile, "kiosk")
        with self.assertRaisesRegex(ValueError, "disagree"):
            LayoutConfig.from_payload({"profile": "mobile", "mode": "desktop"})

    def test_contract_rejects_unknown_and_loose_values(self) -> None:
        bad_payloads = (
            {},
            {"profile": "phone"},
            {"profile": "mobile", "orientation": "sideways"},
            {"profile": "mobile", "insets": "1,2,3"},
            {"profile": "mobile", "insets": "1, 2,3,4"},
            {"profile": "mobile", "insets": {"top": 1, "right": 2, "bottom": 3}},
            {"profile": "mobile", "insets": {"top": True, "right": 2, "bottom": 3, "left": 4}},
            {"profile": "mobile", "extra": "field"},
        )
        for payload in bad_payloads:
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                LayoutConfig.from_payload(payload)

    def test_effective_layout_is_typed_and_bounded(self) -> None:
        parsed = parse_effective(
            "msys.layout.effective.v1;profile=mobile;orientation_policy=auto;"
            "insets_policy=auto;orientation=landscape;"
            "screen=800,480;insets=40,40,0,0;workarea=0,40,760,440;"
            "navigation=right;navigation_region=760,40,40,440"
        )
        self.assertEqual(parsed["screen"], {"width": 800, "height": 480})
        self.assertEqual(parsed["insets"]["right"], 40)
        self.assertEqual(parsed["navigation_edge"], "right")
        self.assertEqual(
            parsed["navigation_region"],
            {"x": 760, "y": 40, "width": 40, "height": 440},
        )
        with self.assertRaisesRegex(ValueError, "exceeds"):
            parse_effective(
                "msys.layout.effective.v1;profile=mobile;orientation_policy=auto;"
                "insets_policy=auto;orientation=portrait;"
                "screen=320,480;insets=0,0,0,0;workarea=1,0,320,480;"
                "navigation=bottom"
            )

    def test_reference_profile_uses_the_canonical_environment_contract(self) -> None:
        profile_path = Path(__file__).parents[1] / "profiles" / "openstick-mobile-spi.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        env = profile["env"]
        self.assertNotIn("DISPLAY", env)
        font_config = (
            Path(__file__).parents[1]
            / "files/share/fontconfig/msys-fonts.conf"
        )
        self.assertEqual(
            env["FONTCONFIG_FILE"],
            "/opt/msys/current/msys-x11-session/files/share/"
            "fontconfig/msys-fonts.conf",
        )
        policy = font_config.read_text(encoding="utf-8")
        self.assertIn('<edit name="dpi"', policy)
        self.assertIn('<edit name="antialias"', policy)
        self.assertIn('<edit name="hinting"', policy)
        self.assertIn('<edit name="embeddedbitmap"', policy)
        self.assertIn('<edit name="rgba"', policy)
        self.assertIn("<const>none</const>", policy)
        config = LayoutConfig(
            profile=env["MSYS_LAYOUT_PROFILE"],
            orientation=env["MSYS_ORIENTATION"],
            insets=Insets.parse(env["MSYS_INSETS"]),
        )
        self.assertEqual(config.profile, "mobile")
        self.assertEqual(config.orientation, "auto")
        self.assertIsNone(config.insets)
        policy = "org.msys.x11.session:window-policy"
        fallback = "org.msys.x11.session:window-policy-python"
        self.assertEqual(profile["roles"]["window-policy"], [policy, fallback])
        self.assertEqual(profile["roles"]["window-manager"], [policy])
        self.assertNotIn(fallback, profile["startup"])
        self.assertLess(
            profile["startup"].index("org.msys.openstick.ch347:x11-spi-touch-output"),
            profile["startup"].index(policy),
        )
        self.assertLess(
            profile["startup"].index(policy),
            profile["startup"].index("org.msys.shell.native:desktop-shell"),
        )
        self.assertNotIn("org.msys.core.install:install-agent", profile["startup"])
        self.assertNotIn("org.msys.core.install:update-agent", profile["startup"])


if __name__ == "__main__":
    unittest.main()
