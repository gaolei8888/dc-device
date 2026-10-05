from __future__ import annotations

import json
from dc_device import DeviceHttpClient

def capability_to_tool(capability: dict) -> dict:
    return {
        "name": capability["name"],
        "description": capability["description"],
        "input_schema": capability.get("parameters", {"type":"object","properties":{}}),
    }

client = DeviceHttpClient("http://127.0.0.1:8787")
manifest = client.manifest()

print("Discovered device:")
print(json.dumps(manifest["device"], indent=2))

print("\nAgent-facing tools:")
for capability in manifest["capabilities"]:
    print(json.dumps(capability_to_tool(capability), indent=2))

print("\nInvoke LED:")
print(json.dumps(client.invoke("led.set", on=True), indent=2))

print("\nInvoke servo:")
print(json.dumps(client.invoke("servo.move", angle=30), indent=2))
