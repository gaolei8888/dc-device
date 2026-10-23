# Camera image to screen: design (DRAFT, not approved)

Status: draft written 2026-10-23 after the owner's answers in session. Nothing here is built.
The owner's decisions are marked **[owner]**; everything else is a proposal.

## Goal

dc-device owns all peripheral I/O **[owner]**: capturing from the camera, sending to the
screen, and the conversion in between are done on the device side, not by DC or its plugin.
The pieces are APIs that can talk to each other **[owner]**, and the boards talk to each
other directly **[owner, this reverses the earlier "boards never connect" decision]**.

Concrete target: the camera on 6740 shows its picture on the screen of 6718 with one request.

## Decided by the owner

1. Boards communicate directly (not only through a host).
2. A landscape camera picture (qvga 320x240) on the portrait screen (240x320) is **scaled to
   fill and cropped** (no black bars, the edges are cut).
3. Order of work: the host-side helper (SDK `show_image`) first; firmware JPEG decoding later
   only if a real need shows.

Items 1 and 3 pull in different directions: 3 is the host-side path, 1 asks for a direct path.
This document keeps both and says what each needs. The owner has not yet said which direct
variant (below) he wants, or whether the host path is only a stepping stone.

## A common image shape

Every peripheral that produces or accepts a picture uses the same value, so the output of
one can be the input of another:

```
image = { content_type: "image/jpeg" | "image/x-rgb565",
          width, height, bytes, sha256, data_base64 }
```

`camera.capture` already returns this for JPEG (contract: 2026-10-22-camera-capture-and-screen-block-contract.md).
`screen.write_block` takes raw RGB565 for one rectangle; the new screen capability below takes a whole `image`.

## Path 1: through a host (SDK helper)

```
host: img = device_a.invoke("camera.capture")        # JPEG, 6740
      show_image(device_b, img)                      # decode, crop-fill, RGB565, 16-row blocks
```

`dc_device.show_image(client, image, pause=0.06, retries=5)`: Pillow decodes, scales to cover
240x320, crops the centre, packs RGB565 big-endian, sends 16-row blocks with `screen.write_block`,
waits at least 50 ms between blocks (`max_rate_hz` 20), retries on `RateLimited` (matched by error
name; the status is 429 only on firmware from c2e09ac on). Pillow is an optional dependency.
Firmware change: none. Work about 1 day. Time per picture about 3 s. Risk low.

This path needs a host in the loop for every picture, so it does not satisfy item 1 by itself.

## Path 2: board to board

Two variants. Both need the same base: a board can call another board's `/invoke`.

**2a. Pull (the screen board fetches).** `screen.show_from` on 6718:
`{peer_url, peer_serial, capability:"camera.capture", arguments:{size, quality}}`. 6718 calls 6740, receives a
JPEG, decodes it, crops, draws. Needs a JPEG decoder and a 100 KB receive buffer on 6718, and an HTTP
client in its firmware.

**2b. Push (the camera board sends).** `camera.send_to_screen` on 6740:
`{peer_url, peer_serial, size}`. 6740 captures **RGB565 directly from the sensor** (OV2640 can output it), scales
and crops in the loop, and calls `screen.write_block` on 6718 for each 16-row strip. No JPEG decoder
anywhere; 6718 needs no new code. A qvga RGB565 frame is 153,600 bytes, so 6740 needs PSRAM for it
(its record says 8 MB; to be measured). 6740 needs an HTTP client.

Recommended direct variant: **2b**, because it avoids a decoder on the board that has the screen and
reuses the verified `screen.write_block` unchanged. It requires camera capture to work first.

### What both need (missing today)

- An HTTP client in firmware (esp_http_client), with timeouts that use the peer's
  `expected_duration_ms.max` plus margin, and 429 / `RateLimited` backoff.
- Finding the peer: **[owner] the host tells a board which board it connects to.** The board does no
  discovery and keeps no pairing table. The host (which knows serial -> current IP) puts `peer_url` and
  the peer's expected `serial` in the request. When an IP changes, the host's next request carries the new one.
  Boards talk directly to each other only for the data (the picture); who talks to whom is the host's decision.
- Check the peer is the intended board: call the peer's `/identity` and compare the `serial` the
  request names, before sending anything.
- A new error vocabulary: `peer_unreachable`, `peer_refused`, `peer_identity_mismatch`, `peer_error`
  (carrying the peer's own `code`).
- Safety: both boards are open to the local network since the access token was dropped; a request that
  makes one board call another adds nothing to that but also does not fix it. Say so in the doc for users.

### Not decided / to measure

- Whether 6740 has PSRAM enabled and usable (needed for 2b); whether 6718 has any (needed for 2a).
- Timing of a whole 2b run: 20 blocks x (about 90 ms + 50 ms spacing) is about 3 s on the screen side, plus capture time (unknown).
- Whether one board holding an HTTP connection to another for seconds starves its own server (the `/health` stalls after boot are still unexplained).

## What the boundary looks like

| Layer | Owns |
|---|---|
| Firmware | peripheral drivers; `camera.capture`, `screen.write_block`, `screen.draw_rect`; (2b) `camera.send_to_screen`; peer HTTP client |
| SDK | `show_image` (path 1); typed helpers around the image value |
| DC / plugin | calls the above and shows results truthfully; no conversion, no chunking |

## Order of work (proposal, needs the owner's go)

1. `show_image` in the SDK with host tests, then check on 6718 with a synthetic picture (no camera needed). About 1 day.
2. Camera capture working on 6740 (blocked on the EV-VSYNC-OVF problem; owner must be present to power-cycle).
3. Peer client in firmware and `camera.send_to_screen` (2b), tested board to board.
4. Only then consider a JPEG decoder (2a). Discovery is out: the host supplies the peer.
