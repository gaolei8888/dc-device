from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Literal, Optional, Union

CapabilityKind = Literal["sensor", "action", "stream"]
CheckState = Literal["ok", "warning", "failed", "skipped"]

class ValidationError(ValueError):
    pass

def _matches_type(value: Any, schema_type: str) -> bool:
    if schema_type == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if schema_type == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if schema_type == "boolean":
        return isinstance(value, bool)
    if schema_type == "string":
        return isinstance(value, str)
    if schema_type == "object":
        return isinstance(value, dict)
    return True

def validate_arguments(schema: Dict[str, Any], arguments: Dict[str, Any]) -> None:
    if not schema:
        return

    required = schema.get("required", [])
    properties = schema.get("properties", {})

    for name in required:
        if name not in arguments:
            raise ValidationError(f"Missing required argument: {name}")

    for name, value in arguments.items():
        rule = properties.get(name)
        if rule is None:
            continue

        expected_type = rule.get("type")
        if expected_type and not _matches_type(value, expected_type):
            raise ValidationError(
                f"Argument {name} must be of type {expected_type}"
            )

        if "enum" in rule and value not in rule["enum"]:
            raise ValidationError(
                f"Argument {name} must be one of {rule['enum']}"
            )

        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if "minimum" in rule and value < rule["minimum"]:
                raise ValidationError(
                    f"Argument {name} must be >= {rule['minimum']}"
                )
            if "maximum" in rule and value > rule["maximum"]:
                raise ValidationError(
                    f"Argument {name} must be <= {rule['maximum']}"
                )

@dataclass
class CheckResult:
    """Outcome of one health check. `code` and `message` describe the issue
    and are only used when `state` is warning or failed."""

    state: CheckState = "ok"
    code: Optional[str] = None
    message: Optional[str] = None

# A check may return a bare state string or a CheckResult.
HealthCheck = Callable[[], Union[CheckState, CheckResult]]

_ISSUE_SEVERITY = {"warning": "warning", "failed": "error"}

def _run_check(name: str, check: HealthCheck) -> CheckResult:
    try:
        outcome = check()
    except Exception as exc:
        return CheckResult("failed", "CHECK_ERROR", f"{exc.__class__.__name__}: {exc}")
    if isinstance(outcome, str):
        outcome = CheckResult(outcome)
    if not isinstance(outcome, CheckResult) or outcome.state not in (
        "ok", "warning", "failed", "skipped"
    ):
        return CheckResult(
            "failed", "CHECK_ERROR", f"Check {name} returned an invalid result"
        )
    return outcome

@dataclass
class Capability:
    name: str
    description: str
    kind: CapabilityKind
    handler: Callable[..., Any]
    parameters: Dict[str, Any] = field(default_factory=dict)
    returns: Dict[str, Any] = field(default_factory=dict)
    safety: Dict[str, Any] = field(default_factory=dict)

@dataclass
class Device:
    device_id: str
    device_type: str
    name: str
    metadata: Dict[str, Any] = field(default_factory=dict)
    capabilities: Dict[str, Capability] = field(default_factory=dict)
    health_checks: Dict[str, HealthCheck] = field(default_factory=dict)
    self_tests: Dict[str, HealthCheck] = field(default_factory=dict)
    diagnostic_info: Dict[str, Callable[[], Any]] = field(default_factory=dict)
    _started: float = field(default_factory=time.monotonic, repr=False)
    _last_self_test: Optional[Dict[str, Any]] = field(default=None, repr=False)
    _self_test_lock: Any = field(default_factory=threading.Lock, repr=False)

    def register(self, capability: Capability) -> None:
        if capability.name in self.capabilities:
            raise ValueError(f"Capability already registered: {capability.name}")
        self.capabilities[capability.name] = capability

    def invoke(self, capability_name: str, **kwargs: Any) -> Any:
        capability = self.capabilities.get(capability_name)
        if capability is None:
            raise KeyError(f"Unknown capability: {capability_name}")

        validate_arguments(capability.parameters, kwargs)
        return capability.handler(**kwargs)

    def manifest(self) -> Dict[str, Any]:
        return {
            "spec_version": "0.1",
            "device": {
                "id": self.device_id,
                "type": self.device_type,
                "name": self.name,
                "metadata": self.metadata,
            },
            "capabilities": [
                {
                    "name": cap.name,
                    "description": cap.description,
                    "kind": cap.kind,
                    "parameters": cap.parameters,
                    "returns": cap.returns,
                    "safety": cap.safety,
                }
                for cap in self.capabilities.values()
            ],
        }

    # --- health contract (docs/health-contract-v0.1.md) ---------------------

    def add_health_check(self, name: str, check: HealthCheck) -> None:
        """Cheap, read-only check run by get_status() and self_test()."""
        self._add_check(self.health_checks, name, check)

    def add_self_test(self, name: str, check: HealthCheck) -> None:
        """Check run only by self_test(). May actuate hardware."""
        self._add_check(self.self_tests, name, check)

    def _add_check(self, registry: Dict[str, HealthCheck], name: str, check: HealthCheck) -> None:
        if name in self.health_checks or name in self.self_tests:
            raise ValueError(f"Check already registered: {name}")
        registry[name] = check

    def add_diagnostic(self, name: str, provider: Callable[[], Any]) -> None:
        if name in self.diagnostic_info:
            raise ValueError(f"Diagnostic already registered: {name}")
        self.diagnostic_info[name] = provider

    def _report(self, checks: Dict[str, HealthCheck]) -> Dict[str, Any]:
        states: Dict[str, str] = {}
        issues: List[Dict[str, Any]] = []
        for name, check in checks.items():
            result = _run_check(name, check)
            states[name] = result.state
            severity = _ISSUE_SEVERITY.get(result.state)
            if severity:
                issues.append({
                    "code": result.code or ("CHECK_FAILED" if severity == "error" else "CHECK_WARNING"),
                    "severity": severity,
                    "check": name,
                    "message": result.message or f"Check {name} reported {result.state}",
                })
        if "failed" in states.values():
            status = "failed"
        elif "warning" in states.values():
            status = "degraded"
        else:
            status = "ok"
        return {
            "device_id": self.device_id,
            "status": status,
            "checks": states,
            "issues": issues,
        }

    def get_status(self) -> Dict[str, Any]:
        return self._report(self.health_checks)

    def self_test(self) -> Dict[str, Any]:
        # Serialized: two concurrent self-tests must not drive hardware at once.
        with self._self_test_lock:
            report = self._report({**self.health_checks, **self.self_tests})
            self._last_self_test = report
            return report

    def diagnostics(self) -> Dict[str, Any]:
        info: Dict[str, Any] = {}
        for name, provider in self.diagnostic_info.items():
            try:
                info[name] = provider()
            except Exception as exc:
                info[name] = {"error": f"{exc.__class__.__name__}: {exc}"}
        return {
            "device_id": self.device_id,
            "status": self.get_status()["status"],
            "uptime_s": round(time.monotonic() - self._started, 3),
            "capability_count": len(self.capabilities),
            "last_self_test": self._last_self_test,
            "info": info,
        }
