from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Literal

CapabilityKind = Literal["sensor", "action", "stream"]

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
