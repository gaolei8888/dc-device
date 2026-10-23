"""show_image: a picture to the 240x320 screen as 16-row RGB565 blocks, paced and retried."""
import base64
import io
import json
from urllib.error import HTTPError

import pytest

PIL = pytest.importorskip("PIL")
from PIL import Image  # noqa: E402

from dc_device import ScreenError, image_to_rgb565, show_image  # noqa: E402


class FakeClient:
    """Records invoke() calls; `script` is a list of per-call outcomes (default: ok)."""

    def __init__(self, script=None):
        self.calls = []
        self.script = list(script or [])

    def invoke(self, capability, **arguments):
        self.calls.append((capability, arguments))
        outcome = self.script.pop(0) if self.script else {"ok": True}
        if isinstance(outcome, Exception):
            raise outcome
        return outcome


def solid(color, size=(320, 240)):
    return Image.new("RGB", size, color)


def rate_limited():
    return {"ok": False, "error": "RateLimited", "message": "too fast"}


def test_solid_red_landscape_becomes_all_red_portrait():
    data = image_to_rgb565(solid((255, 0, 0)))
    assert len(data) == 240 * 320 * 2
    assert data == bytes([0xF8, 0x00]) * (240 * 320)  # big-endian RGB565


def test_cover_and_crop_keeps_the_centre_of_a_landscape_picture():
    img = Image.new("RGB", (320, 240), (255, 0, 0))
    img.paste((0, 0, 255), (160, 0, 320, 240))  # right half blue
    data = image_to_rgb565(img)

    def px(x, y):
        i = (y * 240 + x) * 2
        return data[i], data[i + 1]

    # scaled to 427x320 and cropped to the middle 240 columns: the red/blue edge is at x=120
    assert px(100, 10) == (0xF8, 0x00)
    assert px(140, 10) == (0x00, 0x1F)
    assert px(100, 310) == (0xF8, 0x00)


def test_portrait_picture_of_screen_size_is_not_scaled():
    img = Image.new("RGB", (240, 320), (0, 255, 0))
    assert image_to_rgb565(img) == bytes([0x07, 0xE0]) * (240 * 320)


def test_show_image_sends_twenty_strips_with_pauses():
    client, sleeps = FakeClient(), []
    show_image(client, solid((255, 255, 255)), sleep=sleeps.append)
    assert len(client.calls) == 20
    for i, (cap, args) in enumerate(client.calls):
        assert cap == "screen.write_block"
        assert (args["x"], args["y"], args["width"], args["height"]) == (0, i * 16, 240, 16)
        assert len(base64.b64decode(args["data_base64"])) == 240 * 16 * 2
    assert len(sleeps) == 19 and all(s >= 0.05 for s in sleeps)  # 50 ms is the rate limit


def test_rate_limited_block_is_retried_after_a_pause():
    client, sleeps = FakeClient([rate_limited(), rate_limited()]), []
    show_image(client, solid((0, 0, 0)), sleep=sleeps.append)
    assert len(client.calls) == 22
    assert client.calls[0][1]["y"] == client.calls[1][1]["y"] == client.calls[2][1]["y"] == 0


def test_rate_limit_as_http_error_is_also_retried():
    body = json.dumps({"ok": False, "error": "RateLimited", "message": "x"}).encode()
    err = HTTPError("http://x/invoke", 429, "Too Many Requests", {}, io.BytesIO(body))
    client = FakeClient([err])
    show_image(client, solid((0, 0, 0)), sleep=lambda s: None)
    assert len(client.calls) == 21


def test_gives_up_after_the_retries_and_says_which_block():
    client = FakeClient([rate_limited()] * 10)
    with pytest.raises(ScreenError) as e:
        show_image(client, solid((0, 0, 0)), retries=3, sleep=lambda s: None)
    assert e.value.error == "RateLimited" and e.value.block == 0
    assert len(client.calls) == 4


def test_other_errors_stop_at_once_and_carry_the_code():
    bad = {"ok": False, "error": "InvalidArguments", "code": "bad_block_size", "message": "m"}
    client = FakeClient([{"ok": True}, {"ok": True}, bad])
    with pytest.raises(ScreenError) as e:
        show_image(client, solid((0, 0, 0)), sleep=lambda s: None)
    assert (e.value.error, e.value.code, e.value.block) == ("InvalidArguments", "bad_block_size", 2)
    assert len(client.calls) == 3


def test_accepts_a_camera_capture_result_with_a_jpeg():
    buf = io.BytesIO()
    solid((255, 0, 0)).save(buf, "JPEG")
    capture = {"ok": True, "capability": "camera.capture",
               "result": {"content_type": "image/jpeg", "data_base64": base64.b64encode(buf.getvalue()).decode()}}
    client = FakeClient()
    show_image(client, capture, sleep=lambda s: None)
    first = base64.b64decode(client.calls[0][1]["data_base64"])
    assert first[0] >= 0xF0 and first[1] <= 0x10  # red-ish, big-endian


def test_accepts_encoded_bytes_and_a_path(tmp_path):
    buf = io.BytesIO()
    solid((0, 0, 255)).save(buf, "PNG")
    path = tmp_path / "p.png"
    path.write_bytes(buf.getvalue())
    for source in (buf.getvalue(), path, str(path)):
        client = FakeClient()
        show_image(client, source, sleep=lambda s: None)
        assert len(client.calls) == 20


def test_unreadable_picture_is_a_clear_error_before_anything_is_sent():
    client = FakeClient()
    with pytest.raises(ValueError):
        show_image(client, b"not a picture", sleep=lambda s: None)
    assert client.calls == []
