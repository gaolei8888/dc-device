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


def test_max_rate_hz_rejects_calls_that_are_too_fast():
    import pytest
    from dc_device import Capability, Device, RateLimited

    device = Device("d", "t", "n")
    calls = []
    device.register(Capability("slow.act", "d", "action", lambda: calls.append(1),
                               safety={"max_rate_hz": 2}))
    device.invoke("slow.act")
    with pytest.raises(RateLimited):
        device.invoke("slow.act")
    assert len(calls) == 1


def test_emergency_capability_is_never_rate_limited():
    from dc_device import Capability, Device

    device = Device("d", "t", "n")
    calls = []
    device.register(Capability("stop", "d", "action", lambda: calls.append(1),
                               safety={"max_rate_hz": 1, "emergency": True}))
    for _ in range(5):
        device.invoke("stop")
    assert len(calls) == 5


def test_invalid_arguments_do_not_use_up_the_rate_budget():
    import pytest
    from dc_device import Capability, Device
    from dc_device.device import ValidationError

    device = Device("d", "t", "n")
    device.register(Capability(
        "act", "d", "action", lambda on: None,
        parameters={"type": "object", "properties": {"on": {"type": "boolean"}}, "required": ["on"]},
        safety={"max_rate_hz": 1}))
    with pytest.raises(ValidationError):
        device.invoke("act")
    device.invoke("act", on=True)


def test_register_rejects_inconsistent_safety():
    import pytest
    from dc_device import Capability, Device

    device = Device("d", "t", "n")
    for bad in ({"max_rate_hz": 0}, {"max_rate_hz": "fast"}, {"emergency": 1},
                {"emergency": True, "requires_confirmation": True}):
        with pytest.raises(ValueError):
            device.register(Capability("x", "d", "action", lambda: None, safety=bad))
