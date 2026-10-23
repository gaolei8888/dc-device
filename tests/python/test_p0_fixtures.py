"""The P0 fixtures (tests/fixtures/p0) agree with the schemas and with each other."""
import json
from pathlib import Path

import pytest

from dc_device import normalize_serial

ROOT = Path(__file__).resolve().parents[2]
FIX = ROOT / "tests" / "fixtures" / "p0"
SCHEMAS = ROOT / "schemas"


def fixture(name):
    return json.loads((FIX / name).read_text())


def validator(name):
    jsonschema = pytest.importorskip("jsonschema")
    return jsonschema.Draft202012Validator(json.loads((SCHEMAS / name).read_text()))


def test_identity_matches_schema_and_serial_is_canonical():
    body = fixture("identity.json")["body"]
    validator("device-identity.schema.json").validate(body)
    assert normalize_serial(body["serial"]) == body["serial"]


def test_manifest_matches_schema_and_agrees_with_identity():
    manifest = fixture("manifest.json")["body"]
    validator("device-capability.schema.json").validate(manifest)
    ident = fixture("identity.json")["body"]
    assert manifest["device"]["id"] == ident["device_id"]
    assert manifest["device"]["type"] == ident["type"]
    heap = [c for c in manifest["capabilities"] if c["name"] == "heap.read"]
    assert len(heap) == 1 and heap[0]["kind"] == "sensor"


def test_manifest_lists_only_what_has_evidence():
    # This board has a camera and nothing else a program can see: no speaker, no screen.
    names = [c["name"] for c in fixture("manifest.json")["body"]["capabilities"]]
    assert names == ["heap.read"]
    refused = fixture("invoke_undeclared_capability.json")
    assert refused["status"] == 404 and refused["body"]["error"] == "UnknownCapability"


def test_status_matches_schema():
    validator("device-health.schema.json").validate(fixture("status.json")["body"])


def test_heap_read_result():
    f = fixture("invoke_heap_read.json")
    assert f["status"] == 200 and f["body"]["ok"] is True and f["body"]["capability"] == "heap.read"
    value = f["body"]["result"]["free_heap_bytes"]
    assert isinstance(value, int) and not isinstance(value, bool) and value > 0


@pytest.mark.parametrize("name,status,error", [
    ("invoke_unknown_capability.json", 404, "UnknownCapability"),
    ("invoke_undeclared_capability.json", 404, "UnknownCapability"),
    ("invoke_malformed_json.json", 400, "InvalidRequest"),
])
def test_error_shape(name, status, error):
    f = fixture(name)
    assert f["status"] == status
    assert set(f["body"]) == {"ok", "error", "message"}
    assert f["body"]["ok"] is False and f["body"]["error"] == error and f["body"]["message"]


def test_no_real_mac_in_any_fixture():
    for path in FIX.glob("*.json"):
        assert "B8F862" not in path.read_text(), path.name
