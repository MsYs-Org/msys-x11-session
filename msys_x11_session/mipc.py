"""Small package-local mIPC client used by the X11 session agent.

The window-policy component is a system package and must also run from an
installed package root where the host deliberately removes ``PYTHONPATH``.
Keeping the tiny transport here avoids coupling the display session to the
development checkout of ``msys-sdk`` while preserving the same wire protocol.
"""

from __future__ import annotations

import json
import os
import selectors
import socket
import threading
import time
from pathlib import Path
from typing import Any


MAX_PACKET = 256 * 1024


class MsysClient:
    def __init__(self, sock: socket.socket, component_id: str) -> None:
        self.sock = sock
        self.component_id = component_id
        self.generation = int(os.environ.get("MSYS_GENERATION", "0"))
        self._send_lock = threading.Lock()

    @classmethod
    def from_env(cls) -> "MsysClient":
        fd = int(os.environ["MSYS_CONTROL_FD"])
        return cls(socket.socket(fileno=fd), os.environ.get("MSYS_COMPONENT_ID", "unknown"))

    def send(self, message: dict[str, Any]) -> None:
        data = json.dumps(
            message, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        if len(data) > MAX_PACKET:
            raise ValueError("mIPC packet is too large")
        with self._send_lock:
            self.sock.sendall(data)

    def recv(self, timeout: float | None = None) -> dict[str, Any] | None:
        selector = selectors.DefaultSelector()
        selector.register(self.sock, selectors.EVENT_READ)
        try:
            events = selector.select(timeout)
        finally:
            selector.close()
        if not events:
            return None
        data = self.sock.recv(MAX_PACKET + 1)
        if not data:
            return {"type": "eof"}
        if len(data) > MAX_PACKET:
            raise ValueError("mIPC packet is too large")
        message = json.loads(data.decode("utf-8"))
        if not isinstance(message, dict):
            raise ValueError("mIPC packet must be an object")
        return message

    def hello(self) -> dict[str, Any] | None:
        self.send({
            "type": "hello",
            "component": self.component_id,
            "generation": self.generation,
        })
        return self.recv(timeout=2)

    def ready(self) -> None:
        self.send({"type": "ready"})

    def event(self, topic: str, payload: dict[str, Any] | None = None) -> None:
        self.send({"type": "event", "topic": topic, "payload": payload or {}})

    @staticmethod
    def public_call(
        target: str,
        method: str,
        payload: dict[str, Any] | None = None,
        timeout: float = 5,
        runtime_dir: str | None = None,
    ) -> dict[str, Any]:
        runtime = Path(runtime_dir or os.environ.get("MSYS_RUNTIME_DIR", "/run/msys/main"))
        request = {
            "type": "call",
            "id": 1,
            "target": target,
            "method": method,
            "payload": payload or {},
            "deadline_ms": int(time.monotonic() * 1000 + timeout * 1000),
        }
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(timeout)
            sock.connect(str(runtime / "control.sock"))
            _recv_line(sock, timeout)
            _send_line(sock, request)
            return _recv_line(sock, timeout)


def _send_line(sock: socket.socket, message: dict[str, Any]) -> None:
    data = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(data) > MAX_PACKET:
        raise ValueError("mIPC packet is too large")
    sock.sendall(data + b"\n")


def _recv_line(sock: socket.socket, timeout: float) -> dict[str, Any]:
    sock.settimeout(timeout)
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(MAX_PACKET + 1)
        if not chunk:
            break
        data += chunk
        if len(data) > MAX_PACKET:
            raise ValueError("mIPC packet is too large")
    if not data:
        raise EOFError("empty mIPC public response")
    message = json.loads(data.decode("utf-8"))
    if not isinstance(message, dict):
        raise ValueError("mIPC public response must be an object")
    return message
