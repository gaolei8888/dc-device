import http.client
import json
import threading
import urllib.error

import pytest

from dc_device import Capability, Device, DeviceHttpClient
from dc_device.http_server import make_server

TOKEN = "T" * 43


def make_device(token=TOKEN, clock=None):
    kwargs = {"access_token": token}
    if clock is not None:
        kwargs["clock"] = clock
    d = Device("dev1", "test", "Test Device", **kwargs)
    d.register(Capability("led.set", "Set the LED.", "action", lambda: {"on": True}, {"type": "object", "properties": {}}))
    return d


@pytest.fixture
def serve():
    servers = []

    def start(device):
        server = make_server(device, "127.0.0.1", 0)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        servers.append(server)
        return server.server_address[1]

    yield start
    for s in servers:
        s.shutdown()


def call(port, method, path, body=None, auth=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    headers = {"Content-Type": "application/json"}
    if auth is not None:
        headers["Authorization"] = auth
    conn.request(method, path, body=json.dumps(body) if body is not None else None, headers=headers)
    resp = conn.getresponse()
    data = resp.read().decode()
    out = (resp.status, resp.getheader("WWW-Authenticate"), data)
    conn.close()
    return out


INVOKE = {"capability": "led.set", "arguments": {}}
PROTECTED = [("POST", "/invoke", INVOKE), ("POST", "/self_test", {}), ("GET", "/diagnostics", None)]
OPEN = ["/health", "/identity", "/manifest", "/status"]


def test_protected_routes_need_the_token(serve):
    port = serve(make_device())
    for method, path, body in PROTECTED:
        status, www, data = call(port, method, path, body)
        assert status == 401 and www == "Bearer"
        assert json.loads(data) == {"ok": False, "error": "Unauthorized", "message": "Missing or wrong access token."}
        assert TOKEN not in data
        for bad in ["Bearer nope", "Basic " + TOKEN, "bearer " + TOKEN, "Bearer " + TOKEN + "x", "Bearer " + TOKEN[:-1], "Bearer  " + TOKEN]:
            assert call(port, method, path, body, auth=bad)[0] == 401
        status, www, _ = call(port, method, path, body, auth="Bearer " + TOKEN)
        assert status == 200 and www is None


def test_open_routes_ignore_the_header(serve):
    port = serve(make_device())
    for _ in range(20):
        for path in OPEN:
            assert call(port, "GET", path, auth="Bearer nope")[0] == 200
    # twenty wrong tokens on open routes did not lock anything
    assert call(port, "POST", "/invoke", INVOKE, auth="Bearer " + TOKEN)[0] == 200
    assert call(port, "GET", "/nothing")[0] == 404


def test_identity_reports_auth_only_when_configured(serve):
    port = serve(make_device())
    ident = json.loads(call(port, "GET", "/identity")[2])
    assert ident["auth"] == {"required": True, "paired": True}
    assert TOKEN not in json.dumps(ident)
    plain = serve(make_device(token=None))
    assert "auth" not in json.loads(call(plain, "GET", "/identity")[2])
    assert call(plain, "POST", "/invoke", INVOKE)[0] == 200  # no token configured: open


def test_lockout_after_ten_wrong_tokens(serve):
    now = [100.0]
    port = serve(make_device(clock=lambda: now[0]))
    for _ in range(9):
        assert call(port, "POST", "/invoke", INVOKE, auth="Bearer nope")[0] == 401
    assert call(port, "POST", "/invoke", INVOKE, auth="Bearer " + TOKEN)[0] == 200  # resets the run
    for _ in range(10):
        call(port, "POST", "/invoke", INVOKE, auth="Bearer nope")
    assert call(port, "POST", "/invoke", INVOKE, auth="Bearer " + TOKEN)[0] == 401  # locked out
    now[0] += 29.9
    assert call(port, "POST", "/invoke", INVOKE, auth="Bearer " + TOKEN)[0] == 401
    now[0] += 0.3
    assert call(port, "POST", "/invoke", INVOKE, auth="Bearer " + TOKEN)[0] == 200
    # no header at all is not a guess
    for _ in range(30):
        assert call(port, "POST", "/invoke", INVOKE)[0] == 401
    assert call(port, "POST", "/invoke", INVOKE, auth="Bearer " + TOKEN)[0] == 200


def test_client_sends_the_token(serve):
    port = serve(make_device())
    base = f"http://127.0.0.1:{port}"
    assert DeviceHttpClient(base, token=TOKEN).invoke("led.set")["ok"] is True
    assert DeviceHttpClient(base, token=TOKEN).diagnostics()["device_id"] == "dev1"
    with pytest.raises(urllib.error.HTTPError) as err:
        DeviceHttpClient(base).invoke("led.set")
    assert err.value.code == 401
    assert DeviceHttpClient(base).status()["device_id"] == "dev1"  # open routes need nothing
