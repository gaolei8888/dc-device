from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Literal

CapabilityKind = Literal["sensor", "action", "stream"]

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
