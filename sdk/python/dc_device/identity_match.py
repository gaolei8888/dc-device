"""Reference rule for matching the USB `hello` with the HTTP `GET /identity`.

The USB serial chip's own serial number says nothing about the board. The only link
between a serial port and a network device is the `serial` the firmware reports in both
places (the chip's base MAC, 12 hex digits, upper case, no separators). A host compares
them with this rule and refuses to link on any difference.
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

_SEPARATORS = ":-. \t\r\n"
_HEX = set("0123456789ABCDEF")
# Fields that must agree when both sides carry them.
_FIELDS = ("device_id", "type", "firmware_version")


def normalize_serial(value: Any) -> Optional[str]:
    """Canonical form of a MAC-style serial, or None if it is not one."""
    if not isinstance(value, str):
        return None
    cleaned = "".join(c for c in value if c not in _SEPARATORS).upper()
    if len(cleaned) != 12 or not set(cleaned) <= _HEX:
        return None
    return cleaned


def serials_match(a: Any, b: Any) -> bool:
    """True only if both are valid serials and equal. Two missing serials do not match."""
    na, nb = normalize_serial(a), normalize_serial(b)
    return na is not None and na == nb


def identity_matches_hello(hello: Dict[str, Any], identity: Dict[str, Any]) -> Tuple[bool, List[str]]:
    """Compares a `hello` reply with a `/identity` body. Returns (ok, fields that differ).

    A missing or invalid serial on either side is a mismatch, never a pass.
    """
    differing: List[str] = []
    if not serials_match(hello.get("serial"), identity.get("serial")):
        differing.append("serial")
    for field in _FIELDS:
        if field in hello and field in identity and hello[field] != identity[field]:
            differing.append(field)
    return (not differing, differing)
