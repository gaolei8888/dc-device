"""Access token check (docs/superpowers/specs/2026-10-20-access-token-design.md).

The Linux reference device and the simulator have no USB cable, so the token is given
to the device by whoever starts it. The rules are the same as on the ESP32 board: only
a presented wrong token counts as a failure, and ten in a row lock every protected call
for 30 seconds.
"""

from __future__ import annotations

import hashlib
import hmac
import threading
from typing import Callable

MAX_FAILURES = 10
LOCKOUT_SECONDS = 30.0
_SCHEME = "Bearer "


class AccessGate:
    def __init__(self, token: str, clock: Callable[[], float]):
        self._hash = hashlib.sha256(token.encode("utf-8")).digest()
        self._clock = clock
        self._lock = threading.Lock()
        self._failures = 0
        self._locked_until = None

    def check(self, authorization: str) -> str:
        """Returns "allowed", "unauthorized" or "locked"."""
        with self._lock:
            if self._locked_until is not None:
                if self._clock() < self._locked_until:
                    return "locked"
                self._locked_until = None
                self._failures = 0
            if not authorization:
                return "unauthorized"  # no guess was made; not counted
            scheme_ok = authorization.startswith(_SCHEME)
            candidate = authorization[len(_SCHEME):] if scheme_ok else ""
            digest = hashlib.sha256(candidate.encode("utf-8")).digest()
            if hmac.compare_digest(digest, self._hash) and scheme_ok:
                self._failures = 0
                return "allowed"
            self._failures += 1
            if self._failures >= MAX_FAILURES:
                self._locked_until = self._clock() + LOCKOUT_SECONDS
            return "unauthorized"
