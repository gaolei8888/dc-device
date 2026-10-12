import importlib.util
import json
import threading
from pathlib import Path

import pytest

from dc_device import Capability, CheckResult, Device, DeviceHttpClient, conformance, load_profile
from dc_device.http_server import make_server

ROOT = Path(__file__).resolve().parents[2]
SCHEMAS = ROOT / "schemas"
PROFILES = ROOT / "profiles"


def validator(name):
    jsonschema = pytest.importorskip("jsonschema")
    return jsonschema.Draft202012Validator(json.loads((SCHEMAS / name).read_text()))


# --- identity ----------------------------------------------------------------

def test_minimal_identity():
    d = Device("dev1", "test", "Test Device")
    assert d.identity() == {
        "spec_version": "0.1", "device_id": "dev1", "type": "test", "name": "Test Device",
    }


def test_full_identity_matches_schema():
    d = Device(
        "robot_car_01", "mobile_robot", "Car",
        manufacturer="Acme", model="RC-1", hardware_revision="B",
        firmware_version="0.1.0", serial="AABBCCDDEEFF",
        profile={"id": "esp32-s3-robot-car", "version": "0.1.0"},
    )
    identity = d.identity()
    assert identity["serial"] == "AABBCCDDEEFF"
    assert identity["profile"] == {"id": "esp32-s3-robot-car", "version": "0.1.0"}
    validator("device-identity.schema.json").validate(identity)
    validator("device-identity.schema.json").validate(Device("a", "b", "c").identity())


def test_identity_agrees_with_manifest():
    d = Device("dev1", "test", "Test Device", model="x")
    ident, manifest = d.identity(), d.manifest()["device"]
    assert (ident["device_id"], ident["type"], ident["name"]) == (
        manifest["id"], manifest["type"], manifest["name"],
    )


def test_identity_does_not_leak_into_manifest():
    # The manifest schema forbids extra keys in `device`.
    d = Device("dev1", "test", "T", serial="S", profile={"id": "p", "version": "0.1.0"})
    assert set(d.manifest()["device"]) == {"id", "type", "name", "metadata"}


@pytest.mark.parametrize("bad", [
    {"id": "p"}, {"version": "0.1.0"}, {"id": "", "version": "0.1.0"},
    {"id": "p", "version": 1}, {"id": "p", "version": "0.1.0", "x": "y"},
])
def test_bad_profile_reference_rejected(bad):
    with pytest.raises(ValueError):
        Device("a", "b", "c", profile=bad)


def test_identity_schema_rejects_bad_documents():
    v = validator("device-identity.schema.json")
    jsonschema = pytest.importorskip("jsonschema")
    good = Device("a", "b", "c").identity()
    for mutate in (
        lambda i: i.pop("device_id"),
        lambda i: i.update(spec_version="9"),
        lambda i: i.update(profile={"id": "Bad Id", "version": "0.1.0"}),
        lambda i: i.update(profile={"id": "ok", "version": "1"}),
        lambda i: i.update(serial=""),
    ):
        doc = json.loads(json.dumps(good))
        mutate(doc)
        with pytest.raises(jsonschema.ValidationError):
            v.validate(doc)


# --- profile files -----------------------------------------------------------

@pytest.mark.parametrize("path", sorted(PROFILES.glob("*.profile.json")), ids=lambda p: p.name)
def test_profile_files_are_valid(path):
    profile = load_profile(path)
    validator("device-profile.schema.json").validate(profile)
    assert path.name == f"{profile['profile']['id']}.profile.json"


def test_profile_schema_rejects_bad_documents():
    v = validator("device-profile.schema.json")
    jsonschema = pytest.importorskip("jsonschema")
    good = json.loads((PROFILES / "linux-reference.profile.json").read_text())
    for mutate in (
        lambda p: p.pop("required_capabilities"),
        lambda p: p["profile"].pop("device_type"),
        lambda p: p["required_capabilities"][0].update(kind="gadget"),
        lambda p: p["required_capabilities"][0].update(pins=[1]),   # pins never belong in a profile
        lambda p: p.update(pins={"led": 2}),
        lambda p: p.update(required_health_checks=["a", "a"]),
    ):
        doc = json.loads(json.dumps(good))
        mutate(doc)
        with pytest.raises(jsonschema.ValidationError):
            v.validate(doc)


def test_load_profile_rejects_non_profile(tmp_path):
    f = tmp_path / "x.json"
    f.write_text("[]")
    with pytest.raises(ValueError):
        load_profile(f)


# --- conformance -------------------------------------------------------------

PROFILE = {
    "spec_version": "0.1",
    "profile": {"id": "p", "version": "0.1.0", "name": "P", "device_type": "t"},
    "required_capabilities": [
        {"name": "led.set", "kind": "action"},
        {"name": "temp.read", "kind": "sensor"},
    ],
    "optional_capabilities": [{"name": "cam.capture", "kind": "sensor"}],
    "required_health_checks": ["led"],
}


def make(kinds=None, device_type="t", checks=("led",)):
    kinds = kinds or {"led.set": "action", "temp.read": "sensor"}
    d = Device("d", device_type, "D")
    for name, kind in kinds.items():
        d.register(Capability(name, "x", kind, lambda: 1))
    for c in checks:
        d.add_health_check(c, lambda: "ok")
    return d.manifest(), d.get_status()


def test_conforming_device():
    result = conformance(PROFILE, *make())
    assert result["conforms"] is True
    assert result["profile"] == {"id": "p", "version": "0.1.0"}
    assert result["missing_optional_capabilities"] == ["cam.capture"]  # informational only


def test_missing_capability():
    result = conformance(PROFILE, *make(kinds={"led.set": "action"}))
    assert result["conforms"] is False
    assert result["missing_capabilities"] == ["temp.read"]


def test_kind_mismatch():
    result = conformance(PROFILE, *make(kinds={"led.set": "sensor", "temp.read": "sensor"}))
    assert result["conforms"] is False
    assert result["kind_mismatches"] == [{"name": "led.set", "expected": "action", "actual": "sensor"}]


def test_missing_health_check():
    result = conformance(PROFILE, *make(checks=()))
    assert result["conforms"] is False
    assert result["missing_health_checks"] == ["led"]


def test_wrong_device_type():
    result = conformance(PROFILE, *make(device_type="other"))
    assert result["conforms"] is False
    assert result["type_mismatch"] == {"expected": "t", "actual": "other"}


def test_failing_check_still_conforms():
    # Completeness is not health: /status reports the failure separately.
    manifest, _ = make()
    d = Device("d", "t", "D")
    d.add_health_check("led", lambda: CheckResult("failed", "LED_BAD", "no led"))
    status = d.get_status()
    assert status["status"] == "failed"
    assert conformance(PROFILE, manifest, status)["conforms"] is True


# --- the shipped reference device against its shipped profile -----------------

def test_linux_reference_device_conforms_to_its_profile():
    spec = importlib.util.spec_from_file_location("simulated_device", ROOT / "examples/linux/simulated_device.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    device = module.device
    profile = load_profile(PROFILES / "linux-reference.profile.json")

    assert device.profile == {"id": profile["profile"]["id"], "version": profile["profile"]["version"]}
    result = conformance(profile, device.manifest(), device.get_status())
    assert result["conforms"] is True, result
    validator("device-identity.schema.json").validate(device.identity())


# --- HTTP --------------------------------------------------------------------

def test_http_identity_route_and_client():
    d = Device("dev1", "test", "T", model="m", profile={"id": "p", "version": "0.1.0"})
    d.add_health_check("late", lambda: "failed")  # identity must not depend on health
    server = make_server(d, "127.0.0.1", 0)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        client = DeviceHttpClient(f"http://127.0.0.1:{server.server_address[1]}")
        assert client.identity() == d.identity()
        assert client.manifest()["device"]["id"] == client.identity()["device_id"]
    finally:
        server.shutdown()
        server.server_close()


# --- moving devices need a safe state -----------------------------------------

def _moving_device(with_stop):
    d = Device("d", "t", "D")
    d.register(Capability("led.set", "x", "action", lambda: 1))
    d.register(Capability("temp.read", "x", "sensor", lambda: 1))
    d.register(Capability("arm.move", "x", "action", lambda: 1, safety={"motion": True}))
    d.add_health_check("led", lambda: "ok")
    if with_stop:
        d.register(Capability("arm.stop", "x", "action", lambda: 1, safety={"emergency": True}))
        d.set_safe_state("arm.stop")
    return d


def test_moving_device_without_safe_state_does_not_conform_or_validate():
    d = _moving_device(with_stop=False)
    result = conformance(PROFILE, d.manifest(), d.get_status())
    assert result["conforms"] is False
    assert result["missing_safe_state"] is True
    with pytest.raises(ValueError):
        d.validate_safety()


def test_moving_device_with_safe_state_conforms():
    d = _moving_device(with_stop=True)
    result = conformance(PROFILE, d.manifest(), d.get_status())
    assert result["missing_safe_state"] is False
    assert result["conforms"] is True
    d.validate_safety()


def test_safe_state_must_be_an_emergency_capability_in_the_manifest():
    d = _moving_device(with_stop=False)
    manifest = d.manifest()
    manifest["safety_kernel"] = {"safe_state": "led.set"}  # not an emergency capability
    assert conformance(PROFILE, manifest, d.get_status())["missing_safe_state"] is True


def test_non_moving_device_needs_no_safe_state():
    result = conformance(PROFILE, *make())
    assert result["missing_safe_state"] is False
