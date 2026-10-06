from __future__ import annotations

import random

from dc_device import Capability, CheckResult, Device, serve

state = {"led_on": False, "servo_angle": 90.0}

def read_temperature() -> dict:
    return {"celsius": round(22 + random.random() * 4, 2)}

def set_led(on: bool) -> dict:
    state["led_on"] = bool(on)
    return {"on": state["led_on"]}

def move_servo(angle: float) -> dict:
    if not 0 <= angle <= 180:
        raise ValueError("angle must be between 0 and 180")
    state["servo_angle"] = float(angle)
    return {"angle": state["servo_angle"]}

device = Device(
    device_id="linux_ref_01",
    device_type="linux_reference",
    name="dc-device Linux Reference Device",
)

device.register(Capability(
    name="sensor.temperature",
    description="Read ambient temperature.",
    kind="sensor",
    handler=read_temperature,
    returns={"type":"object","properties":{"celsius":{"type":"number"}}},
))

device.register(Capability(
    name="led.set",
    description="Turn the status LED on or off.",
    kind="action",
    handler=set_led,
    parameters={"type":"object","properties":{"on":{"type":"boolean"}},"required":["on"]},
))

device.register(Capability(
    name="servo.move",
    description="Move servo to an absolute angle.",
    kind="action",
    handler=move_servo,
    parameters={
        "type":"object",
        "properties":{"angle":{"type":"number","minimum":0,"maximum":180}},
        "required":["angle"],
    },
    safety={"max_rate_hz":5},
))

device.add_health_check("connection", lambda: "ok")

def servo_sweep() -> CheckResult:
    for angle in (0, 180, 90):
        move_servo(angle)
    return CheckResult("ok")

device.add_self_test("servo_sweep", servo_sweep)
device.add_diagnostic("firmware", lambda: "linux-reference-0.1.0")

if __name__ == "__main__":
    serve(device, host="127.0.0.1", port=8787)
