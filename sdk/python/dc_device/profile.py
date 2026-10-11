"""Device-class profiles (docs/identity-and-profile-v0.1.md)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, Union


def load_profile(path: Union[str, Path]) -> Dict[str, Any]:
    """Load a profile file. Full validation is the job of
    schemas/device-profile.schema.json; this only guards the keys that
    conformance() relies on."""
    profile = json.loads(Path(path).read_text(encoding="utf-8"))
    if (
        not isinstance(profile, dict)
        or not isinstance(profile.get("profile"), dict)
        or not isinstance(profile.get("required_capabilities"), list)
    ):
        raise ValueError(f"Not a device profile: {path}")
    return profile


def conformance(
    profile: Dict[str, Any], manifest: Dict[str, Any], status: Dict[str, Any]
) -> Dict[str, Any]:
    """Check that a device provides everything a profile requires.

    This is about completeness, not health: a check that is present but
    failing still counts as present, and /status reports that separately.
    `conforms` ignores optional capabilities.
    """
    capabilities = {c["name"]: c for c in manifest.get("capabilities", [])}

    missing = []
    kind_mismatches = []
    for required in profile["required_capabilities"]:
        actual = capabilities.get(required["name"])
        if actual is None:
            missing.append(required["name"])
        elif actual.get("kind") != required["kind"]:
            kind_mismatches.append({
                "name": required["name"],
                "expected": required["kind"],
                "actual": actual.get("kind"),
            })

    optional_missing = [
        c["name"]
        for c in profile.get("optional_capabilities", [])
        if c["name"] not in capabilities
    ]

    present_checks = status.get("checks", {})
    missing_checks = [
        name for name in profile.get("required_health_checks", []) if name not in present_checks
    ]

    expected_type = profile["profile"]["device_type"]
    actual_type = manifest.get("device", {}).get("type")
    type_mismatch = (
        None if actual_type == expected_type
        else {"expected": expected_type, "actual": actual_type}
    )

    return {
        "profile": {
            "id": profile["profile"]["id"],
            "version": profile["profile"]["version"],
        },
        "conforms": not (missing or kind_mismatches or missing_checks or type_mismatch),
        "type_mismatch": type_mismatch,
        "missing_capabilities": missing,
        "kind_mismatches": kind_mismatches,
        "missing_health_checks": missing_checks,
        "missing_optional_capabilities": optional_missing,
    }
