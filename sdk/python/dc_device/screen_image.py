"""Send a picture to a device screen: scale to fill, crop, RGB565, 16-row blocks, paced.

The device accepts at most 7680 pixel bytes per `screen.write_block` and at most 20 calls a
second (`safety.max_rate_hz`), so one picture is 20 paced calls. Pillow is needed to decode and
scale; it is imported only when a picture is sent.
"""
from __future__ import annotations

import base64
import io
import json
import time
from pathlib import Path
from typing import Any, Callable
from urllib.error import HTTPError

SCREEN_WIDTH = 240
SCREEN_HEIGHT = 320
BLOCK_ROWS = 16  # 240 x 16 x 2 bytes = 7680, the device's per-call limit
MIN_PAUSE_S = 0.1  # the device allows 20 calls a second; DC lets an action through at most 10 a second


class ScreenError(Exception):
    """A block was refused. `block` is the 0-based block index; `error`/`code` are the device's."""

    def __init__(self, error: str, message: str = "", code: str = "", block: int = -1):
        super().__init__(f"{error}{' (' + code + ')' if code else ''} at block {block}: {message}")
        self.error, self.message, self.code, self.block = error, message, code, block


def _pillow():
    try:
        from PIL import Image
    except ImportError as e:  # pragma: no cover - depends on the environment
        raise ImportError("show_image needs Pillow: pip install Pillow") from e
    return Image


def _open(image: Any):
    Image = _pillow()
    if isinstance(image, Image.Image):
        return image.convert("RGB")
    if isinstance(image, dict):  # a camera.capture reply, with or without the envelope
        image = image.get("result", image)
        image = base64.b64decode(image["data_base64"])
    if isinstance(image, (str, Path)):
        image = Path(image).read_bytes()
    try:
        picture = Image.open(io.BytesIO(bytes(image)))
        picture.load()
    except Exception as e:
        raise ValueError(f"cannot read the picture: {e}") from e
    return picture.convert("RGB")


def image_to_rgb565(image: Any, width: int = SCREEN_WIDTH, height: int = SCREEN_HEIGHT) -> bytes:
    """Scale to cover width x height, crop the middle, pack as big-endian RGB565 (high byte first)."""
    Image = _pillow()
    picture = _open(image)
    scale = max(width / picture.width, height / picture.height)
    scaled_w, scaled_h = max(width, round(picture.width * scale)), max(height, round(picture.height * scale))
    if (scaled_w, scaled_h) != picture.size:
        picture = picture.resize((scaled_w, scaled_h), Image.LANCZOS)
    left, top = (scaled_w - width) // 2, (scaled_h - height) // 2
    picture = picture.crop((left, top, left + width, top + height))
    raw = picture.tobytes()  # R, G, B per pixel
    out = bytearray()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += bytes((v >> 8, v & 0xFF))
    return bytes(out)


def _reply(client: Any, block: int, arguments: dict) -> dict:
    try:
        return client.invoke("screen.write_block", **arguments)
    except HTTPError as e:  # the client raises on 4xx/5xx; the body still names the error
        try:
            return json.loads(e.read().decode("utf-8"))
        except Exception:
            raise ScreenError("HttpError", f"HTTP {e.code}", block=block) from e


def show_image(client: Any, image: Any, *, pause: float = MIN_PAUSE_S, retries: int = 5,
               sleep: Callable[[float], None] = time.sleep) -> None:
    """Show `image` (PIL image, encoded bytes, path, or a camera.capture reply) on the screen.

    A RateLimited block is retried up to `retries` times after a pause; any other refusal raises
    ScreenError at once. Matching is by the error name, which is reliable on every firmware;
    the HTTP status is 429 only from firmware c2e09ac on.
    """
    pixels = image_to_rgb565(image)
    row_bytes = SCREEN_WIDTH * 2
    blocks = SCREEN_HEIGHT // BLOCK_ROWS
    for i in range(blocks):
        if i:
            sleep(pause)
        chunk = pixels[i * BLOCK_ROWS * row_bytes:(i + 1) * BLOCK_ROWS * row_bytes]
        arguments = {"x": 0, "y": i * BLOCK_ROWS, "width": SCREEN_WIDTH, "height": BLOCK_ROWS,
                     "data_base64": base64.b64encode(chunk).decode("ascii")}
        for attempt in range(retries + 1):
            reply = _reply(client, i, arguments)
            if reply.get("ok", True) is not False:
                break
            if reply.get("error") != "RateLimited" or attempt == retries:
                raise ScreenError(reply.get("error", "DeviceError"), reply.get("message", ""),
                                  reply.get("code", ""), i)
            sleep(pause)
