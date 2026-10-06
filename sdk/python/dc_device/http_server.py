from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import urlparse

from .device import Device


class DeviceRequestHandler(BaseHTTPRequestHandler):
    device: Device | None = None

    def _send_json(self, status: int, payload: Any) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        return

    def do_GET(self) -> None:
        if self.device is None:
            self._send_json(500, {"error": "device_not_configured"})
            return

        path = urlparse(self.path).path

        if path == "/health":
            self._send_json(200, {"status": "ok", "device_id": self.device.device_id})
            return

        if path == "/manifest":
            self._send_json(200, self.device.manifest())
            return

        if path == "/status":
            self._send_json(200, self.device.get_status())
            return

        if path == "/diagnostics":
            self._send_json(200, self.device.diagnostics())
            return

        self._send_json(404, {"error": "not_found"})

    def do_POST(self) -> None:
        if self.device is None:
            self._send_json(500, {"error": "device_not_configured"})
            return

        path = urlparse(self.path).path
        if path == "/self_test":
            self._send_json(200, self.device.self_test())
            return
        if path != "/invoke":
            self._send_json(404, {"error": "not_found"})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length else b"{}"
            payload = json.loads(raw.decode("utf-8"))
            capability = payload["capability"]
            arguments = payload.get("arguments", {})
            result = self.device.invoke(capability, **arguments)
            self._send_json(
                200,
                {
                    "ok": True,
                    "capability": capability,
                    "result": result,
                },
            )
        except KeyError as exc:
            self._send_json(404, {"ok": False, "error": str(exc)})
        except Exception as exc:
            self._send_json(
                400,
                {"ok": False, "error": exc.__class__.__name__, "message": str(exc)},
            )


def make_server(device: Device, host: str = "0.0.0.0", port: int = 8787) -> ThreadingHTTPServer:
    handler = type(
        "BoundDeviceRequestHandler",
        (DeviceRequestHandler,),
        {"device": device},
    )
    return ThreadingHTTPServer((host, port), handler)


def serve(device: Device, host: str = "0.0.0.0", port: int = 8787) -> None:
    server = make_server(device, host, port)
    print(f"dc-device serving {device.device_id} on http://{host}:{port}")
    server.serve_forever()
