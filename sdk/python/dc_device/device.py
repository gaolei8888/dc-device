from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Literal, Optional, Union

CapabilityKind = Literal["sensor", "action", "stream"]
CheckState = Literal["ok", "warning", "failed", "skipped"]

class ValidationError(ValueError):
    pass

class RateLimited(RuntimeError):
    """The capability was invoked faster than its safety.max_rate_hz allows."""

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

def _check_safety(capability: "Capability") -> None:
    safety = capability.safety
    rate = safety.get("max_rate_hz")
    if "max_rate_hz" in safety and (
        isinstance(rate, bool) or not isinstance(rate, (int, float)) or rate <= 0
    ):
        raise ValueError(f"{capability.name}: safety.max_rate_hz must be a number > 0")
    duration = safety.get("max_duration_ms")
    if "max_duration_ms" in safety and (
        isinstance(duration, bool) or not isinstance(duration, int) or duration <= 0
    ):
        raise ValueError(f"{capability.name}: safety.max_duration_ms must be an integer > 0")
    if duration is not None and safety.get("emergency"):
        raise ValueError(f"{capability.name}: an emergency capability cannot be time limited")
    for flag in ("emergency", "requires_confirmation", "motion"):
        if flag in safety and not isinstance(safety[flag], bool):
            raise ValueError(f"{capability.name}: safety.{flag} must be a boolean")
    if safety.get("emergency") and safety.get("requires_confirmation"):
        raise ValueError(
            f"{capability.name}: an emergency capability cannot require confirmation"
        )

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
    # Identity (docs/identity-and-profile-v0.1.md). All optional.
    manufacturer: Optional[str] = None
    model: Optional[str] = None
    hardware_revision: Optional[str] = None
    firmware_version: Optional[str] = None
    serial: Optional[str] = None
    profile: Optional[Dict[str, str]] = None  # {"id": ..., "version": ...}
    health_checks: Dict[str, HealthCheck] = field(default_factory=dict)
    self_tests: Dict[str, HealthCheck] = field(default_factory=dict)
    diagnostic_info: Dict[str, Callable[[], Any]] = field(default_factory=dict)
    _started: float = field(default_factory=time.monotonic, repr=False)
    _last_invoked: Dict[str, float] = field(default_factory=dict, repr=False)
    _rate_lock: Any = field(default_factory=threading.Lock, repr=False)
    safe_state: Optional[str] = None
    safe_on_boot: bool = False
    clock: Callable[[], float] = field(default=time.monotonic, repr=False)
    _deadlines: Dict[str, float] = field(default_factory=dict, repr=False)
    _kernel_lock: Any = field(default_factory=threading.Lock, repr=False)
    _last_self_test: Optional[Dict[str, Any]] = field(default=None, repr=False)
    _self_test_lock: Any = field(default_factory=threading.Lock, repr=False)

    def __post_init__(self) -> None:
        if self.profile is not None:
            if set(self.profile) != {"id", "version"} or not all(
                isinstance(v, str) and v for v in self.profile.values()
            ):
                raise ValueError('profile must be {"id": str, "version": str}')

    def register(self, capability: Capability) -> None:
        if capability.name in self.capabilities:
            raise ValueError(f"Capability already registered: {capability.name}")
        _check_safety(capability)
        self.capabilities[capability.name] = capability

    def invoke(self, capability_name: str, **kwargs: Any) -> Any:
        capability = self.capabilities.get(capability_name)
        if capability is None:
            raise KeyError(f"Unknown capability: {capability_name}")

        validate_arguments(capability.parameters, kwargs)
        self._check_rate(capability)
        result = capability.handler(**kwargs)
        with self._kernel_lock:
            if capability.name == self.safe_state:
                self._deadlines.clear()
            duration = capability.safety.get("max_duration_ms")
            if duration is not None:
                self._deadlines[capability.name] = self.clock() + duration / 1000.0
        return result

    def set_safe_state(self, capability_name: str, safe_on_boot: bool = False) -> None:
        """Name the emergency capability that brings the device to its safe state.

        See docs/safety-kernel-v0.1.md. Register the capability first.
        """
        capability = self.capabilities.get(capability_name)
        if capability is None or capability.safety.get("emergency") is not True:
            raise ValueError(f"{capability_name} must be a registered emergency capability")
        if capability.parameters.get("required"):
            raise ValueError(f"{capability_name} must not require arguments")
        self.safe_state = capability_name
        self.safe_on_boot = bool(safe_on_boot)

    def validate_safety(self) -> None:
        """Raise ValueError if a time-limited or moving capability has no safe
        state to fall back to (docs/safety-kernel-v0.1.md)."""
        if self.safe_state is not None:
            return
        for cap in self.capabilities.values():
            if "max_duration_ms" in cap.safety:
                raise ValueError(
                    f"{cap.name} declares max_duration_ms but no safe state is set"
                )
            if cap.safety.get("motion") is True:
                raise ValueError(
                    f"{cap.name} declares motion but no safe state is set"
                )

    def tick(self) -> bool:
        """Run the safe state if any time-limited capability has outlived its
        max_duration_ms. Call often (the HTTP server does so every 50 ms).
        Returns True if the safe state ran."""
        with self._kernel_lock:
            now = self.clock()
            if self.safe_state is None or not any(d <= now for d in self._deadlines.values()):
                return False
            self._deadlines.clear()
        self.capabilities[self.safe_state].handler()
        return True

    def _check_rate(self, capability: Capability) -> None:
        # docs/safety-semantics-v0.1.md: max_rate_hz is a minimum interval
        # between accepted invocations; emergency capabilities are exempt.
        rate = capability.safety.get("max_rate_hz")
        if capability.safety.get("emergency") is True or not isinstance(rate, (int, float)) \
                or isinstance(rate, bool) or rate <= 0:
            return
        interval = 1.0 / rate
        with self._rate_lock:
            now = time.monotonic()
            last = self._last_invoked.get(capability.name)
            if last is not None and now - last < interval:
                wait_ms = int((interval - (now - last)) * 1000) + 1
                raise RateLimited(
                    f"{capability.name} is limited to {rate:g} Hz; retry in {wait_ms} ms"
                )
            self._last_invoked[capability.name] = now

    def manifest(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {
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
        if self.safe_state is not None:
            out["safety_kernel"] = {
                "safe_state": self.safe_state,
                "safe_on_boot": self.safe_on_boot,
            }
        return out

    def identity(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {
            "spec_version": "0.1",
            "device_id": self.device_id,
            "type": self.device_type,
            "name": self.name,
        }
        for key in ("manufacturer", "model", "hardware_revision", "firmware_version", "serial"):
            value = getattr(self, key)
            if value:
                out[key] = value
        if self.profile:
            out["profile"] = dict(self.profile)
        return out

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
