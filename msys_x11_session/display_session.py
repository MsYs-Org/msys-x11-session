"""MSYS display-session v1 live ready/state contract.

Display providers publish one atomic JSON document after the X server answers a
real probe.  Consumers therefore see the effective DISPLAY, root geometry and
input transform rather than profile wishes or hard-coded panel dimensions.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence


SCHEMA = "msys.display-session.v1"
IDENTITY_MATRIX = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
DISPLAY_PATTERN = re.compile(r"^:[0-9]+(?:\.[0-9]+)?$")
DIMENSIONS_PATTERN = re.compile(r"\bdimensions:\s*([0-9]+)x([0-9]+)\s+pixels\b")
DEPTH_PATTERN = re.compile(r"\bdepth of root window:\s*([0-9]+)\s+planes\b", re.IGNORECASE)
XINPUT_MATRIX_PATTERN = re.compile(
    r"Coordinate Transformation Matrix[^:]*:\s*([^\r\n]+)", re.IGNORECASE
)


class DisplaySessionError(ValueError):
    pass


def _enabled(value: Any, default: bool = False) -> bool:
    if value is None or value == "":
        return default
    return str(value).strip().lower() not in {"0", "false", "no", "off"}


def parse_xdpyinfo(text: str) -> dict[str, int]:
    dimensions = DIMENSIONS_PATTERN.search(text)
    if dimensions is None:
        raise DisplaySessionError("xdpyinfo did not report root dimensions")
    width, height = (int(dimensions.group(1)), int(dimensions.group(2)))
    if width < 1 or height < 1 or width > 65535 or height > 65535:
        raise DisplaySessionError("X11 root dimensions are outside supported bounds")
    depth_match = DEPTH_PATTERN.search(text)
    depth = int(depth_match.group(1)) if depth_match else 0
    if depth < 0 or depth > 128:
        raise DisplaySessionError("X11 root depth is outside supported bounds")
    return {"width": width, "height": height, "depth": depth}


def parse_matrix(value: str | Sequence[Any]) -> tuple[float, ...]:
    raw: Sequence[Any]
    if isinstance(value, str):
        raw = [part.strip() for part in value.replace(",", " ").split()]
    else:
        raw = value
    if len(raw) != 9:
        raise DisplaySessionError("input transform must contain exactly nine values")
    try:
        matrix = tuple(float(part) for part in raw)
    except (TypeError, ValueError, OverflowError) as exc:
        raise DisplaySessionError("input transform contains a non-number") from exc
    if not all(math.isfinite(part) and abs(part) <= 1000000 for part in matrix):
        raise DisplaySessionError("input transform contains an invalid number")
    if abs(matrix[8]) < 1e-12:
        raise DisplaySessionError("input transform has an invalid homogeneous scale")
    determinant = (
        matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
        - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
        + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6])
    )
    if abs(determinant) < 1e-12:
        raise DisplaySessionError("input transform must be invertible")
    return matrix


def ch347_transform(env: Mapping[str, str]) -> tuple[float, ...]:
    """Return the exact normalized affine transform used by the CH347 mapper."""

    swap = _enabled(env.get("CH347_TOUCH_SWAP_XY"))
    invert_x = _enabled(env.get("CH347_TOUCH_INVERT_X"))
    invert_y = _enabled(env.get("CH347_TOUCH_INVERT_Y"))
    x_scale, x_offset = (-1.0, 1.0) if invert_x else (1.0, 0.0)
    y_scale, y_offset = (-1.0, 1.0) if invert_y else (1.0, 0.0)
    if swap:
        calibrated = (0.0, x_scale, x_offset, y_scale, 0.0, y_offset, 0.0, 0.0, 1.0)
    else:
        calibrated = (x_scale, 0.0, x_offset, 0.0, y_scale, y_offset, 0.0, 0.0, 1.0)
    rotation = str(env.get("CH347_DISPLAY_ROTATION", "normal")).strip().lower()
    rotations = {
        "normal": (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
        "right": (0.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 1.0),
        "inverted": (-1.0, 0.0, 1.0, 0.0, -1.0, 1.0, 0.0, 0.0, 1.0),
        "left": (0.0, -1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0),
    }
    if rotation not in rotations:
        raise DisplaySessionError("unsupported CH347 display rotation")
    physical_to_logical = rotations[rotation]
    return tuple(
        sum(physical_to_logical[row * 3 + item] * calibrated[item * 3 + column]
            for item in range(3))
        for row in range(3)
        for column in range(3)
    )


def parse_xinput_transform(text: str) -> tuple[float, ...]:
    match = XINPUT_MATRIX_PATTERN.search(text)
    if match is None:
        raise DisplaySessionError("xinput did not report a coordinate transform")
    return parse_matrix(match.group(1))


def _matrix_json(matrix: Sequence[float] | None) -> list[int | float] | None:
    if matrix is None:
        return None
    return [int(value) if float(value).is_integer() else float(value) for value in matrix]


Runner = Callable[..., subprocess.CompletedProcess[str]]


def effective_input_transform(
    env: Mapping[str, str],
    *,
    runner: Runner = subprocess.run,
) -> dict[str, Any]:
    mode = str(env.get("MSYS_DISPLAY_INPUT_MODE", "auto")).strip().lower()
    explicit = str(env.get("MSYS_INPUT_TRANSFORM", "")).strip()
    device = str(env.get("MSYS_INPUT_DEVICE", "")).strip()
    if explicit:
        if mode == "none":
            raise DisplaySessionError("input mode none cannot declare an input transform")
        matrix = parse_matrix(explicit)
        return {
            "enabled": mode != "none",
            "mode": mode if mode != "auto" else "configured",
            "device": device or None,
            "space": "normalized-display",
            "matrix": _matrix_json(matrix),
            "source": "provider-effective-environment",
            "verified": True,
        }
    if mode == "auto" and "CH347_TOUCH" in env:
        mode = "ch347-direct"
    if mode in {"ch347-direct", "ch347-xtest"}:
        enabled = _enabled(env.get("CH347_TOUCH"), default=True)
        return {
            "enabled": enabled,
            "mode": mode,
            "device": device or "CH347 XPT2046",
            "space": "normalized-display",
            "matrix": _matrix_json(ch347_transform(env)) if enabled else None,
            "source": (
                "ch347-xtest-effective"
                if mode == "ch347-xtest"
                else "ch347-direct-effective"
            ),
            "verified": True,
        }
    if mode == "xinput" and device:
        display = str(env.get("DISPLAY", ""))
        try:
            result = runner(
                ["xinput", "list-props", device],
                env={**os.environ, **dict(env), "DISPLAY": display},
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=2,
                check=False,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
            raise DisplaySessionError(f"cannot query XInput device {device}: {exc}") from exc
        if result.returncode != 0:
            raise DisplaySessionError(
                f"cannot query XInput device {device}: {result.stderr.strip() or result.returncode}"
            )
        return {
            "enabled": True,
            "mode": mode,
            "device": device,
            "space": "normalized-display",
            "matrix": _matrix_json(parse_xinput_transform(result.stdout)),
            "source": "xinput-coordinate-transformation-matrix",
            "verified": True,
        }
    if mode not in {"auto", "none", "xinput"}:
        raise DisplaySessionError(f"unsupported display input mode: {mode}")
    return {
        "enabled": False,
        "mode": "none",
        "device": device or None,
        "space": "normalized-display",
        "matrix": None,
        "source": "no-provider-owned-input",
        "verified": True,
    }


def observe_display_session(
    *,
    display: str,
    provider: str,
    env: Mapping[str, str] | None = None,
    runner: Runner = subprocess.run,
) -> dict[str, Any]:
    display = str(display).strip()
    provider = str(provider).strip()
    if not DISPLAY_PATTERN.fullmatch(display):
        raise DisplaySessionError(f"invalid X11 DISPLAY: {display or '<empty>'}")
    if not provider or len(provider) > 256 or "\x00" in provider:
        raise DisplaySessionError("provider must be a bounded component id")
    values = dict(os.environ if env is None else env)
    values["DISPLAY"] = display
    try:
        result = runner(
            ["xdpyinfo"],
            env=values,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=2,
            check=False,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        raise DisplaySessionError(f"cannot probe X11 display {display}: {exc}") from exc
    if result.returncode != 0:
        raise DisplaySessionError(
            f"cannot probe X11 display {display}: {result.stderr.strip() or result.returncode}"
        )
    geometry = parse_xdpyinfo(result.stdout)
    generation_value = str(values.get("MSYS_GENERATION", "0"))
    try:
        generation = max(0, int(generation_value))
    except ValueError:
        generation = 0
    return {
        "schema": SCHEMA,
        "state": "ready",
        "provider": provider,
        "generation": generation,
        "display": display,
        "geometry": geometry,
        "input_transform": effective_input_transform(values, runner=runner),
        "observed_at_unix_ms": int(time.time() * 1000),
    }


def validate_state(document: object) -> dict[str, Any]:
    if not isinstance(document, dict) or document.get("schema") != SCHEMA:
        raise DisplaySessionError(f"display session must use {SCHEMA}")
    if document.get("state") != "ready":
        raise DisplaySessionError("display session state must be ready")
    provider = document.get("provider")
    if not isinstance(provider, str) or not provider or len(provider) > 256 or "\x00" in provider:
        raise DisplaySessionError("display session provider is invalid")
    generation = document.get("generation")
    if isinstance(generation, bool) or not isinstance(generation, int) or generation < 0:
        raise DisplaySessionError("display session generation must be a non-negative integer")
    if not DISPLAY_PATTERN.fullmatch(str(document.get("display", ""))):
        raise DisplaySessionError("display session contains an invalid DISPLAY")
    geometry = document.get("geometry")
    if not isinstance(geometry, dict):
        raise DisplaySessionError("display session geometry must be an object")
    for key in ("width", "height", "depth"):
        if isinstance(geometry.get(key), bool) or not isinstance(geometry.get(key), int):
            raise DisplaySessionError(f"display session geometry.{key} must be an integer")
    if geometry["width"] < 1 or geometry["height"] < 1 or geometry["depth"] < 0:
        raise DisplaySessionError("display session geometry is outside supported bounds")
    transform = document.get("input_transform")
    if not isinstance(transform, dict) or not isinstance(transform.get("enabled"), bool):
        raise DisplaySessionError("display session input_transform is invalid")
    if transform.get("space") != "normalized-display":
        raise DisplaySessionError("display session input transform space is invalid")
    if not isinstance(transform.get("mode"), str) or not transform["mode"]:
        raise DisplaySessionError("display session input transform mode is invalid")
    if not isinstance(transform.get("source"), str) or not transform["source"]:
        raise DisplaySessionError("display session input transform source is invalid")
    if not isinstance(transform.get("verified"), bool) or not transform["verified"]:
        raise DisplaySessionError("display session input transform must be verified")
    matrix = transform.get("matrix")
    if transform["enabled"]:
        parse_matrix(matrix)
    elif matrix is not None:
        raise DisplaySessionError("disabled input transform must have a null matrix")
    observed = document.get("observed_at_unix_ms")
    if isinstance(observed, bool) or not isinstance(observed, int) or observed < 0:
        raise DisplaySessionError("display session observation timestamp is invalid")
    return document


def write_state(path: Path, document: Mapping[str, Any]) -> Path:
    candidate = dict(document)
    validate_state(candidate)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    # A display-session document is state, not a heartbeat.  Providers probe
    # the X server periodically to detect a lost display, but rewriting an
    # otherwise identical document makes consumers treat it as a layout
    # change.  In particular, a new observed_at timestamp used to make every
    # liveness probe trigger an atomic replace and a full X11 layout sync.
    # Keep the original observation (and inode/mtime) when only volatile
    # observation metadata differs.
    try:
        current = load_state(path)
    except DisplaySessionError:
        current = None
    if current is not None and _state_semantically_equal(current, candidate):
        return path

    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(candidate, stream, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise
    return path


def _state_semantically_equal(
    current: Mapping[str, Any], candidate: Mapping[str, Any]
) -> bool:
    """Compare display state without its observation-only timestamp."""

    def stable(document: Mapping[str, Any]) -> dict[str, Any]:
        value = dict(document)
        value.pop("observed_at_unix_ms", None)
        return value

    return stable(current) == stable(candidate)


def load_state(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise DisplaySessionError(f"cannot read display session state: {exc}") from exc
    return validate_state(document)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="publish an MSYS display-session v1 state")
    parser.add_argument("--display", default=os.environ.get("DISPLAY_ID") or os.environ.get("DISPLAY", ":0"))
    parser.add_argument("--provider", default=os.environ.get("MSYS_COMPONENT_ID", "external-display-provider"))
    parser.add_argument("--state-file", default=os.environ.get("MSYS_DISPLAY_SESSION_STATE_FILE") or os.environ.get("MSYS_X11_READY_FILE", ""))
    parser.add_argument(
        "--input-mode",
        choices=("auto", "none", "ch347-direct", "ch347-xtest", "xinput"),
    )
    parser.add_argument("--input-device")
    args = parser.parse_args(argv)
    values = dict(os.environ)
    if args.input_mode:
        values["MSYS_DISPLAY_INPUT_MODE"] = args.input_mode
    if args.input_device:
        values["MSYS_INPUT_DEVICE"] = args.input_device
    try:
        state = observe_display_session(
            display=args.display,
            provider=args.provider,
            env=values,
        )
        if args.state_file:
            write_state(Path(args.state_file), state)
        print(json.dumps(state, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
    except DisplaySessionError as exc:
        print(f"msys-display-session: {exc}", file=os.sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
