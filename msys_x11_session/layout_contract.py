from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping


CONFIG_SCHEMA = "msys.layout.v1"
EFFECTIVE_SCHEMA = "msys.layout.effective.v1"
PROFILES = frozenset({"mobile", "kiosk", "desktop"})
ORIENTATION_POLICIES = frozenset({"auto", "portrait", "landscape"})
EFFECTIVE_ORIENTATIONS = frozenset({"portrait", "landscape"})
NAVIGATION_EDGES = frozenset({"bottom", "right"})
MAX_EDGE = 2**31 - 1


def _integer(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{field} must be an integer")
    if value < 0 or value > MAX_EDGE:
        raise ValueError(f"{field} is outside 0..{MAX_EDGE}")
    return value


@dataclass(frozen=True)
class Insets:
    top: int
    right: int
    bottom: int
    left: int

    @classmethod
    def parse(cls, value: Any) -> Insets | None:
        if value == "auto":
            return None
        if isinstance(value, str):
            fields = value.split(",")
            if len(fields) != 4 or any(not field.isdigit() for field in fields):
                raise ValueError("insets must be auto or top,right,bottom,left")
            numbers = [int(field) for field in fields]
            return cls(*(_integer(number, "insets") for number in numbers))
        if isinstance(value, Mapping):
            expected = {"top", "right", "bottom", "left"}
            if set(value) != expected:
                raise ValueError("insets object requires exactly top/right/bottom/left")
            return cls(*(_integer(value[field], f"insets.{field}") for field in ("top", "right", "bottom", "left")))
        raise ValueError("insets must be auto, a four-integer string, or an edge object")

    def wire(self) -> str:
        return f"{self.top},{self.right},{self.bottom},{self.left}"

    def payload(self) -> dict[str, int]:
        return {
            "top": self.top,
            "right": self.right,
            "bottom": self.bottom,
            "left": self.left,
        }


@dataclass(frozen=True)
class LayoutConfig:
    profile: str = "mobile"
    orientation: str = "auto"
    insets: Insets | None = None

    def __post_init__(self) -> None:
        if self.profile not in PROFILES:
            raise ValueError(f"unknown layout profile: {self.profile}")
        if self.orientation not in ORIENTATION_POLICIES:
            raise ValueError(f"unknown orientation policy: {self.orientation}")

    @classmethod
    def from_payload(cls, payload: Mapping[str, Any]) -> LayoutConfig:
        allowed = {"profile", "mode", "orientation", "insets"}
        unexpected = set(payload) - allowed
        if unexpected:
            raise ValueError(f"unexpected layout fields: {', '.join(sorted(unexpected))}")
        profile = payload.get("profile", payload.get("mode"))
        if not isinstance(profile, str) or not profile:
            raise ValueError("layout profile is required")
        if "profile" in payload and "mode" in payload and payload["profile"] != payload["mode"]:
            raise ValueError("profile and legacy mode disagree")
        orientation = payload.get("orientation", "auto")
        if not isinstance(orientation, str):
            raise ValueError("orientation must be a string")
        insets = Insets.parse(payload.get("insets", "auto"))
        return cls(profile=profile, orientation=orientation, insets=insets)

    @classmethod
    def decode(cls, text: str) -> LayoutConfig:
        fields = _fields(text, CONFIG_SCHEMA, ("profile", "orientation", "insets"))
        return cls(
            profile=fields["profile"],
            orientation=fields["orientation"],
            insets=Insets.parse(fields["insets"]),
        )

    def insets_wire(self) -> str:
        return "auto" if self.insets is None else self.insets.wire()

    def encode(self) -> str:
        return (
            f"{CONFIG_SCHEMA};profile={self.profile};orientation={self.orientation};"
            f"insets={self.insets_wire()}"
        )

    def command_arguments(self) -> list[str]:
        return [self.profile, self.orientation, self.insets_wire()]


def _fields(text: str, schema: str, expected: tuple[str, ...]) -> dict[str, str]:
    parts = text.strip().split(";")
    if not parts or parts[0] != schema or len(parts) != len(expected) + 1:
        raise ValueError(f"invalid {schema} field count or schema")
    result: dict[str, str] = {}
    for part, name in zip(parts[1:], expected, strict=True):
        key, separator, value = part.partition("=")
        if separator != "=" or key != name or not value:
            raise ValueError(f"invalid {schema} field: {part}")
        result[key] = value
    return result


def _number_tuple(value: str, count: int, field: str) -> tuple[int, ...]:
    parts = value.split(",")
    if len(parts) != count or any(not part.isdigit() for part in parts):
        raise ValueError(f"{field} must contain {count} non-negative integers")
    return tuple(_integer(int(part), field) for part in parts)


def parse_effective(text: str) -> dict[str, Any]:
    required = (
        "profile",
        "orientation_policy",
        "insets_policy",
        "orientation",
        "screen",
        "insets",
        "workarea",
        "navigation",
    )
    optional = ("navigation_region",)
    parts = text.strip().split(";")
    if not parts or parts[0] != EFFECTIVE_SCHEMA:
        raise ValueError(f"invalid {EFFECTIVE_SCHEMA} schema")
    fields: dict[str, str] = {}
    for part in parts[1:]:
        key, separator, value = part.partition("=")
        if separator != "=" or not value or key in fields:
            raise ValueError(f"invalid {EFFECTIVE_SCHEMA} field: {part}")
        if key not in required and key not in optional:
            raise ValueError(f"unexpected {EFFECTIVE_SCHEMA} field: {key}")
        fields[key] = value
    missing = [name for name in required if name not in fields]
    if missing:
        raise ValueError(
            f"missing {EFFECTIVE_SCHEMA} fields: {', '.join(missing)}"
        )
    if fields["profile"] not in PROFILES:
        raise ValueError(f"unknown effective profile: {fields['profile']}")
    if fields["orientation"] not in EFFECTIVE_ORIENTATIONS:
        raise ValueError(f"invalid effective orientation: {fields['orientation']}")
    if fields["orientation_policy"] not in ORIENTATION_POLICIES:
        raise ValueError(f"invalid orientation policy: {fields['orientation_policy']}")
    requested_insets = Insets.parse(fields["insets_policy"])
    if fields["navigation"] not in NAVIGATION_EDGES:
        raise ValueError(f"invalid navigation edge: {fields['navigation']}")
    width, height = _number_tuple(fields["screen"], 2, "screen")
    top, right, bottom, left = _number_tuple(fields["insets"], 4, "insets")
    x, y, work_width, work_height = _number_tuple(fields["workarea"], 4, "workarea")
    if width < 1 or height < 1 or work_width < 1 or work_height < 1:
        raise ValueError("screen and workarea dimensions must be positive")
    if x + work_width > width or y + work_height > height:
        raise ValueError("workarea exceeds the X11 root window")
    expected_workarea = (left, top, width - left - right, height - top - bottom)
    if (x, y, work_width, work_height) != expected_workarea:
        raise ValueError("workarea is inconsistent with screen and insets")
    navigation_region: tuple[int, int, int, int] | None = None
    if "navigation_region" in fields:
        navigation_region = _number_tuple(
            fields["navigation_region"], 4, "navigation_region"
        )
    elif fields["navigation"] == "right" and right > 0:
        navigation_region = (width - right, top, right, height - top - bottom)
    elif fields["navigation"] == "bottom" and bottom > 0:
        navigation_region = (left, height - bottom, width - left - right, bottom)
    if navigation_region is not None:
        navigation_x, navigation_y, navigation_width, navigation_height = navigation_region
        if navigation_width < 1 or navigation_height < 1:
            raise ValueError("navigation_region dimensions must be positive")
        if (
            navigation_x + navigation_width > width
            or navigation_y + navigation_height > height
        ):
            raise ValueError("navigation_region exceeds the X11 root window")
        if fields["navigation"] == "right" and navigation_x + navigation_width != width:
            raise ValueError("right navigation_region does not touch the right edge")
        if fields["navigation"] == "bottom" and navigation_y + navigation_height != height:
            raise ValueError("bottom navigation_region does not touch the bottom edge")
    return {
        "schema": EFFECTIVE_SCHEMA,
        "profile": fields["profile"],
        "orientation_policy": fields["orientation_policy"],
        "insets_policy": fields["insets_policy"],
        "requested_insets": None if requested_insets is None else requested_insets.payload(),
        "orientation": fields["orientation"],
        "screen": {"width": width, "height": height},
        "insets": {"top": top, "right": right, "bottom": bottom, "left": left},
        "workarea": {"x": x, "y": y, "width": work_width, "height": work_height},
        "navigation_edge": fields["navigation"],
        "navigation_region": (
            None
            if navigation_region is None
            else {
                "x": navigation_region[0],
                "y": navigation_region[1],
                "width": navigation_region[2],
                "height": navigation_region[3],
            }
        ),
    }
