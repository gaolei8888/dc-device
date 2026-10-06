from __future__ import annotations

import json
from typing import Any
from urllib.request import Request, urlopen


class DeviceHttpClient:
    def __init__(self, base_url: str):
        self.base_url = base_url.rstrip("/")

    def _get(self, path: str) -> Any:
        with urlopen(self.base_url + path, timeout=5) as response:
            return json.loads(response.read().decode("utf-8"))

    def _post(self, path: str, payload: dict[str, Any], timeout: float = 10) -> Any:
        body = json.dumps(payload).encode("utf-8")
        request = Request(
            self.base_url + path,
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))

    def health(self) -> Any:
        return self._get("/health")

    def status(self) -> Any:
        return self._get("/status")

    def diagnostics(self) -> Any:
        return self._get("/diagnostics")

    def self_test(self) -> Any:
        # Self-tests can take a while (they may move hardware).
        return self._post("/self_test", {}, timeout=30)

    def manifest(self) -> Any:
        return self._get("/manifest")

    def invoke(self, capability: str, **arguments: Any) -> Any:
        return self._post(
            "/invoke",
            {
                "capability": capability,
                "arguments": arguments,
            },
        )
