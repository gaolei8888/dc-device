# camera.capture, screen.write_block and expected duration - contract draft

Status: APPROVED by the owner on 2026-10-22 (camera.capture, screen.write_block with
screen.draw_rect, and the expected-duration field with error `code`). Built so far: the error
`code`, `expected_duration_ms`, `screen.draw_rect` and `screen.write_block`. Not built:
`camera.capture` (the camera driver has never delivered a frame on 6740).
Boards are named by serial tail: **6740** = camera only; **6718** = screen (240x320) and
speaker. Goal behind it: a picture taken by 6740 shown on 6718, relayed by the host (DC);
the boards never talk to each other.

What is a fact and what is a guess is marked. Anything marked GUESS has not been measured.

## What exists today (facts from the code)

- `POST /invoke` answers 200 `{"ok":true,"capability":..,"result":..}`. Bad arguments are
  400 `InvalidArguments`; a device that could not act is 500 `DeviceError` with a text
  message only. There is **no machine-readable error code** besides that name.
- A POST body larger than `max_body_bytes` (default **2048**) is refused with 413. A
  picture block sent to the screen does not fit; the limit has to be raised for that
  one capability (see "Limits").
- The camera driver has never delivered a frame on 6740 (`EV-VSYNC-OVF`). The sensor
  (OV2640, I2C 0x30) is detected. `camera.capture` is a promise that cannot be kept until
  that is solved; it is declared only after a frame has really been taken (below).
- 6740 has 8 MB PSRAM (from the board record); the released firmware does not enable it.
  Free internal heap on a running board was 224,700 bytes (reported by `heap.read`).
- Access token: `/invoke` is protected. These capabilities need the token like any other.

## 1. camera.capture (first)

`kind: "sensor"`. It takes one picture and returns it inside the `/invoke` result. It moves
nothing, so it has no safe state and no `max_duration_ms`.

Arguments:

| name | type | rule |
|---|---|---|
| `size` | string | one of `"qqvga"` (160x120), `"qvga"` (320x240, default), `"vga"` (640x480) |
| `quality` | integer | 10..90, default 60; higher is better and larger. Mapped to the driver's scale by the firmware |

Result (inside `result`):

```json
{"content_type":"image/jpeg","width":320,"height":240,"bytes":14233,
 "sha256":"<64 hex of the JPEG bytes>","data_base64":"<standard base64, no line breaks>"}
```

- One frame, one request. No stream, no multi-frame. The frame is taken after the call
  arrives (not an old buffer), so it is current.
- `bytes` is the JPEG size; `data_base64` decodes to exactly `bytes` bytes whose SHA-256
  is `sha256`. A host must check both; a mismatch means the picture is damaged and must
  not be shown as good.
- Size limit: the JPEG must be at most **100,000 bytes** (the whole response then stays
  under 150 KB with base64). If the picture would be larger the firmware retakes it once
  at lower quality; if still too large it fails with `image_too_large`. Typical
  `qvga` at quality 60 is 10-20 KB (GUESS, not measured on this board).
- Memory: `vga` or a large JPEG held together with its base64 copy does not fit in
  224 KB of internal heap. Until PSRAM is enabled and measured, the firmware offers only
  `qqvga` and `qvga`; `vga` is declared only when PSRAM is present. (The `size` enum in
  the manifest lists exactly what this firmware supports.)

Errors. `/invoke` failures gain an additive field `code` (stable, machine-readable); the
existing `error` and `message` stay:
`{"ok":false,"error":"DeviceError","code":"camera_no_frame","message":"..."}`.

| code | when |
|---|---|
| `camera_not_found` | the sensor does not answer on the camera bus |
| `camera_no_frame` | the sensor answers but no frame arrived within the wait |
| `camera_busy` | another capture is running |
| `image_too_large` | still over the size limit at the lowest quality |
| `out_of_memory` | the frame or its base64 copy could not be allocated |

Rules:
- Safe to repeat (idempotent, no side effect); a host may retry once on `camera_no_frame`.
- Honest declaration: `camera.capture` is in `/manifest` only when the sensor was detected
  on the bus **and** one frame was successfully taken and judged usable since boot (the
  frame check in `dc_device_image_analysis`: not all black, not saturated, not constant).
  Detection alone does not declare it. Its self-test result is `ok` only on a usable frame,
  otherwise `failed` with the reason; a sensor present but never delivering is `failed`,
  not `unverified`.
- A picture of a home is private. It crosses the network in cleartext in this step (HTTP,
  token-protected but not encrypted); that limit is stated in the token design. Whether DC
  asks the user each time is DC's decision (`requires_confirmation` is a caller rule).
- Expected duration (section 3): typical 600 ms, max 4000 ms (GUESS).

Camera bring-up is a research task, not a contract item: the order of attempts for
`EV-VSYNC-OVF` (clock pin and frequency setting for a module that has its own oscillator,
pixel format JPEG vs raw, frame buffer location and PSRAM, SCCB start order, watchdogs
around the capture) is for the builder. Every attempt must start from a USB power cycle,
because the sensor keeps streaming through an MCU reset.

## 2. screen.draw_rect and screen.write_block (6718)

Screen: 240 wide, **320 high** (owner's answer), RGB565, origin top-left, orientation as
the current `screen.fill_test`. Declared only when the screen is confirmed (honest
manifest rules); pins are the known map, not measured.

`screen.draw_rect` `{x, y, width, height, color}`: fills a rectangle. `color` is an
integer 0..65535 (RGB565). The rectangle must lie inside the screen or the call is
`InvalidArguments`.

`screen.write_block` `{x, y, width, height, data_base64}`: writes pixels into the
rectangle.

- Pixel format: RGB565, **big-endian on the wire (high byte first)**, row by row, left to
  right, top to bottom - the byte order `screen.fill_test` already sends to this panel.
- `data_base64` decodes to exactly `width * height * 2` bytes, or the call is
  `InvalidArguments`.
- Block limit: at most **7,680 pixel bytes** per call (e.g. 240 x 16 rows = 3,840 pixels),
  which is 10,240 base64 characters. A full 240x320 screen is 20 calls of 16 rows. The
  firmware raises `max_body_bytes` to 12,288 for this build only; other builds keep 2048.
- The panel stays initialised between calls, so a picture can arrive in pieces. A call
  that leaves the screen half drawn is the host's concern; nothing blocks on the device
  waiting for the rest.
- Rate and duration: each call GUESS 80-150 ms (HTTP time dominates; the SPI transfer of
  7,680 bytes at 20 MHz is about 3 ms). A full screen in 20 calls is therefore about 2-3 s
  (GUESS, to be measured). Declares `safety.max_rate_hz: 20`.
- A screen does not move anything: `kind: "action"`, no `max_duration_ms`, no safe state.
  Existing `screen.fill_test` stays.
- Converting a camera picture to RGB565 blocks (decode JPEG, scale to 240x320 or fit, convert,
  cut into rows) is done by the host, not by either board.

Errors use the same `code` field: `screen_not_ready` (panel did not start; 500), `out_of_range`
(well-formed but outside the screen; 400 `InvalidArguments`), `bad_block_size` (data is not
width x height x 2 bytes, or over 7680; 400), `bad_base64` (data_base64 is not strict standard
base64; 400).

## 3. Expected duration (manifest)

Each capability may carry an optional, read-only field in `/manifest`:

```json
"expected_duration_ms": {"typical": 600, "max": 4000}
```

- It tells a host how long to wait before calling the result unknown. It enforces nothing and
  is **not** `safety.max_duration_ms` (that one means "the action runs at most this long,
  then the safe state runs" and brings in the safety kernel).
- Host rule: wait at least `max` (plus its own network margin); if absent, use the host's
  default. A timeout does not prove the action did not happen: a host reads state again before
  deciding.
- `screen.fill_test`: typical 3400, max 6000 (typical is from reading the code, not measured).

## What this changes elsewhere

- `/invoke` error body: additive `code`. Existing hosts ignore it.
- `max_body_bytes` raised for the camera_board build (12,288).
- `/manifest`: additive optional `expected_duration_ms` on capabilities; the capability
  schema allows additional properties at that level (the builder confirms).
- Schemas and docs: `docs/http-transport-v0.1.md` (error code, body limit),
  `schemas/device-capability.schema.json`.

## Left to the owner

1. Approve section 1 (camera.capture) as written, or change: sizes, quality range, the 100 KB
   limit, base64 in JSON, the declare-after-a-real-frame rule.
2. Approve section 2 (block size 7,680 bytes, big-endian, 12,288 body limit).
3. Approve the `expected_duration_ms` field and the additive `code`.

## Left to the builder

Camera driver bring-up, PSRAM enablement and its measured effect, JPEG vs raw capture, how
the frame buffer and base64 string are held without a second full copy, the exact
timing of each capability on the board (to replace the GUESS values), how the panel is kept
open between calls, tests for every error code and for argument bounds.
