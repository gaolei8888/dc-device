import json
import threading
from pathlib import Path

import pytest

from dc_device import Capability, CheckResult, Device, DeviceHttpClient
from dc_device.http_server import make_server

SCHEMAS = Path(__file__).resolve().parents[2] / "schemas"


def make_device() -> Device:
    return Device("dev1", "test", "Test Device")


# --- status derivation -------------------------------------------------------

def test_no_checks_is_ok():
    report = make_device().get_status()
    assert report == {"device_id": "dev1", "status": "ok", "checks": {}, "issues": []}


def test_warning_makes_degraded_with_issue():
    d = make_device()
    d.add_health_check("connection", lambda: "ok")
    d.add_health_check("fps", lambda: CheckResult("warning", "LOW_FPS", "fps below expected threshold"))
    report = d.get_status()
    assert report["status"] == "degraded"
    assert report["checks"] == {"connection": "ok", "fps": "warning"}
    assert report["issues"] == [
        {"code": "LOW_FPS", "severity": "warning", "check": "fps",
         "message": "fps below expected threshold"}
    ]


def test_failed_beats_warning():
    d = make_device()
    d.add_health_check("a", lambda: "warning")
    d.add_health_check("b", lambda: CheckResult("failed", "NO_STREAM", "stream lost"))
    report = d.get_status()
    assert report["status"] == "failed"
    assert {i["severity"] for i in report["issues"]} == {"warning", "error"}


def test_skipped_and_ok_raise_no_issue():
    d = make_device()
    d.add_health_check("a", lambda: "skipped")
    d.add_health_check("b", lambda: "ok")
    report = d.get_status()
    assert report["status"] == "ok"
    assert report["issues"] == []


def test_default_issue_code_and_message():
    d = make_device()
    d.add_health_check("a", lambda: "failed")
    issue = d.get_status()["issues"][0]
    assert issue["code"] == "CHECK_FAILED"
    assert issue["message"]


def test_raising_check_is_failed_not_crash():
    d = make_device()

    def boom():
        raise RuntimeError("sensor bus stuck")

    d.add_health_check("bus", boom)
    report = d.get_status()
    assert report["status"] == "failed"
    assert report["issues"][0]["code"] == "CHECK_ERROR"
    assert "sensor bus stuck" in report["issues"][0]["message"]


def test_invalid_check_result_is_failed():
    d = make_device()
    d.add_health_check("a", lambda: "great")
    d.add_health_check("b", lambda: 42)
    report = d.get_status()
    assert report["checks"] == {"a": "failed", "b": "failed"}
    assert all(i["code"] == "CHECK_ERROR" for i in report["issues"])


def test_duplicate_check_names_rejected_across_registries():
    d = make_device()
    d.add_health_check("x", lambda: "ok")
    with pytest.raises(ValueError):
        d.add_health_check("x", lambda: "ok")
    with pytest.raises(ValueError):
        d.add_self_test("x", lambda: "ok")


# --- self test ---------------------------------------------------------------

def test_status_does_not_run_self_tests_but_self_test_runs_both():
    d = make_device()
    ran = []
    d.add_health_check("cheap", lambda: ran.append("cheap") or "ok")
    d.add_self_test("sweep", lambda: ran.append("sweep") or "ok")

    assert d.get_status()["checks"] == {"cheap": "ok"}
    assert ran == ["cheap"]

    report = d.self_test()
    assert report["checks"] == {"cheap": "ok", "sweep": "ok"}
    assert ran == ["cheap", "cheap", "sweep"]


def test_self_tests_are_serialized():
    d = make_device()
    active = []
    overlap = []
    gate = threading.Event()

    def slow():
        active.append(1)
        if len(active) > 1:
            overlap.append(True)
        gate.wait(0.05)
        active.pop()
        return "ok"

    d.add_self_test("slow", slow)
    threads = [threading.Thread(target=d.self_test) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert overlap == []


# --- diagnostics -------------------------------------------------------------

def test_diagnostics_shape_and_last_self_test():
    d = make_device()
    d.register(Capability("echo", "Echo.", "action", lambda: 1))
    d.add_self_test("sweep", lambda: "ok")
    d.add_diagnostic("firmware", lambda: "0.1.0")

    before = d.diagnostics()
    assert before["device_id"] == "dev1"
    assert before["status"] == "ok"
    assert before["capability_count"] == 1
    assert before["last_self_test"] is None
    assert before["info"] == {"firmware": "0.1.0"}
    assert before["uptime_s"] >= 0

    d.self_test()
    after = d.diagnostics()
    assert after["last_self_test"]["checks"] == {"sweep": "ok"}


def test_diagnostics_never_runs_self_test():
    d = make_device()
    ran = []
    d.add_self_test("sweep", lambda: ran.append(1) or "ok")
    d.diagnostics()
    assert ran == []


def test_failing_diagnostic_provider_is_contained():
    d = make_device()

    def bad():
        raise OSError("no uart")

    d.add_diagnostic("uart", bad)
    d.add_diagnostic("fw", lambda: "1")
    info = d.diagnostics()["info"]
    assert info["fw"] == "1"
    assert "no uart" in info["uart"]["error"]


# --- schemas -----------------------------------------------------------------

def _validators():
    jsonschema = pytest.importorskip("jsonschema")
    referencing = pytest.importorskip("referencing")
    health = json.loads((SCHEMAS / "device-health.schema.json").read_text())
    diag = json.loads((SCHEMAS / "device-diagnostics.schema.json").read_text())
    registry = referencing.Registry().with_resource(
        health["$id"], referencing.Resource.from_contents(health)
    )
    return (
        jsonschema.Draft202012Validator(health, registry=registry),
        jsonschema.Draft202012Validator(diag, registry=registry),
    )


def test_reports_validate_against_schemas():
    health_v, diag_v = _validators()
    d = make_device()
    d.add_health_check("ok", lambda: "ok")
    d.add_health_check("warn", lambda: CheckResult("warning", "LOW_FPS", "slow"))
    d.add_health_check("bad", lambda: CheckResult("failed", "NO_STREAM", "gone"))
    d.add_health_check("raises", lambda: 1 / 0)
    d.add_self_test("skip", lambda: "skipped")

    health_v.validate(make_device().get_status())
    health_v.validate(d.get_status())
    report = d.self_test()
    health_v.validate(report)
    diag_v.validate(make_device().diagnostics())  # last_self_test null
    diag_v.validate(d.diagnostics())              # last_self_test populated


def test_schema_rejects_bad_report():
    health_v, _ = _validators()
    jsonschema = pytest.importorskip("jsonschema")
    with pytest.raises(jsonschema.ValidationError):
        health_v.validate({"device_id": "x", "status": "READY", "checks": {}, "issues": []})
    with pytest.raises(jsonschema.ValidationError):
        health_v.validate({"device_id": "x", "status": "ok", "checks": {"a": "meh"}, "issues": []})


# --- HTTP --------------------------------------------------------------------

@pytest.fixture
def served():
    d = make_device()
    d.add_health_check("connection", lambda: "ok")
    d.add_self_test("sweep", lambda: "ok")
    d.add_diagnostic("firmware", lambda: "0.1.0")
    server = make_server(d, "127.0.0.1", 0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    client = DeviceHttpClient(f"http://127.0.0.1:{server.server_address[1]}")
    yield d, client
    server.shutdown()
    server.server_close()


def test_http_health_stays_liveness(served):
    d, client = served
    d.add_health_check("late", lambda: "failed")  # broken device still answers /health ok
    assert client.health() == {"status": "ok", "device_id": "dev1"}


def test_http_status(served):
    _, client = served
    report = client.status()
    assert report["status"] == "ok"
    assert report["checks"] == {"connection": "ok"}


def test_http_failed_report_is_still_200(served):
    d, client = served
    d.add_health_check("bus", lambda: "failed")
    assert client.status()["status"] == "failed"  # urlopen would raise on non-2xx


def test_http_self_test_then_diagnostics(served):
    _, client = served
    assert client.diagnostics()["last_self_test"] is None
    report = client.self_test()
    assert report["checks"] == {"connection": "ok", "sweep": "ok"}
    diag = client.diagnostics()
    assert diag["last_self_test"] == report
    assert diag["info"] == {"firmware": "0.1.0"}


def test_http_wrong_methods_are_404(served):
    import urllib.error
    import urllib.request

    _, client = served
    for method, path in (("POST", "/status"), ("POST", "/diagnostics"), ("GET", "/self_test")):
        req = urllib.request.Request(
            client.base_url + path,
            data=b"{}" if method == "POST" else None,
            method=method,
        )
        with pytest.raises(urllib.error.HTTPError) as err:
            urllib.request.urlopen(req)
        assert err.value.code == 404
