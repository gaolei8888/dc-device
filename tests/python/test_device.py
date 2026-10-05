from dc_device import Capability, Device

def test_register_and_invoke():
    device = Device("dev1", "test", "Test Device")
    device.register(Capability(
        name="echo",
        description="Echo a value.",
        kind="action",
        handler=lambda value: {"value": value},
    ))
    assert device.invoke("echo", value=123) == {"value": 123}

def test_manifest_contains_capability():
    device = Device("dev1", "test", "Test Device")
    device.register(Capability(
        name="sensor.read",
        description="Read a value.",
        kind="sensor",
        handler=lambda: 1,
    ))
    manifest = device.manifest()
    assert manifest["spec_version"] == "0.1"
    assert manifest["device"]["id"] == "dev1"
    assert manifest["capabilities"][0]["name"] == "sensor.read"


def test_argument_validation():
    device = Device("dev1", "test", "Test Device")
    device.register(Capability(
        name="servo.move",
        description="Move a servo.",
        kind="action",
        handler=lambda angle: {"angle": angle},
        parameters={
            "type":"object",
            "properties":{"angle":{"type":"number","minimum":0,"maximum":180}},
            "required":["angle"],
        },
    ))

    assert device.invoke("servo.move", angle=90) == {"angle": 90}

    try:
        device.invoke("servo.move", angle=200)
        assert False, "expected validation error"
    except ValueError:
        pass
