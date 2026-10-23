# OTA over the LAN, no access code: design (DRAFT, not approved)

Status: draft, 2026-10-23. Nothing here is built. It replaces
`2026-10-20-ota-design.md` (cloud service, back office, canary rollout, customer switch behind
an access code), which assumed things the owner has now dropped.

## Decided by the owner

- No access code (token) for OTA. **[owner]**
- Everything is on the LAN. **[owner]** There is no cloud service, no back office, no per-board keys
  issued by a server, no canary percent. The host (DC) that already finds the boards and knows
  their IPs is the only thing that talks to a board.

- USB must also work as an update path. **[owner]** OTA adds a way in; it removes none. A board with
  no Wi-Fi, a wrong Wi-Fi, or a firmware that cannot join the network is still brought back by USB.

- Until OTA is done and rehearsed, boards are updated over USB as today. **[owner]** Nothing in the
  OTA work may change how a board is flashed now, and no board moves to the OTA layout before the
  OTA path has passed the rehearsal below.

## USB stays (what it means here)

1. **esptool flashing keeps working** on the new layout: `scripts/build-release.sh` writes the
   offsets for the new partition table in `manifest.json` (bootloader, partition table, `otadata`,
   app), so the same three-or-four-segment `write-flash` flow applies. Writing the app to slot 0 leaves
   `otadata` pointing at it (the release flow writes `otadata` blank, which boots slot 0).
2. **The same signed image goes over USB too (proposal, owner to confirm):** a new USB setup command
   `ota` (`DCSETUP {"cmd":"ota","size":N,"sha256":"..."}` followed by chunked frames) feeds the same
   verify-and-switch path as `POST /ota/upload`. It needs no Wi-Fi, and it checks the signature like the
   LAN path. esptool remains the way to recover a board with a broken app, because it does not depend on
   the running firmware.
3. Neither path may leave the other unusable: the USB setup protocol (hello, wifi, parts, declare) keeps
   working during and after an OTA update.

## Intent

Today a new firmware needs a USB cable and a person next to the board. Over the LAN the host
should be able to send a new firmware to a board it can reach, the board installs it, and a bad
firmware rolls back by itself. Boards that are physically near the owner (the three development
boards, a classroom) do not need a USB cable for ordinary updates.

## Goals

1. The host sends a firmware image to one board over HTTP; the board writes it to the inactive
   slot, verifies it, reboots into it, and keeps it only if it proves it works.
2. A bad firmware does not leave the board unreachable: it rolls back by itself.
3. **Only firmware signed by us installs.** With no access code, anyone on the LAN can call the
   update endpoint, so the signature is the only protection against a stranger's image.
4. An update never starts while a time-limited capability (motor, servo, ...) is running.

## Non-goals

- Any cloud, any server we run, any rollout policy; the host decides which boards to update and when.
- Bootloader and partition table over the air (USB only, and the first move to the OTA layout is a USB flash).
- Delta updates; secure boot; flash encryption.
- Authentication of the caller. A stranger on the same Wi-Fi can **try** to update a board; the
  signature makes that fail, but they can still waste the board's time (see risks).

## Anti-goals

- An unsigned or wrongly signed image installs.
- A firmware that cannot rejoin Wi-Fi or cannot answer `/health` stays installed.
- An update starts while a motor moves.

## Interface (proposal)

- `GET /ota/status` -> `{state, current, previous, last_result, slot}`; `state` is idle, receiving,
  verifying, rebooting, or waiting_confirm. Read-only.
- `POST /ota/upload`: body is the signed image (about 1 MB), `Content-Length` required, streamed to flash
  in chunks (the server today buffers a whole body in RAM and caps it at 2 KB / 12 KB, so this needs a
  streaming handler, not a larger limit). Answers 409 `Busy` while a time-limited capability runs or
  another upload is in progress. Errors carry `code`: `bad_signature`, `bad_size`, `incompatible`,
  `downgrade`, `flash_error`, `not_ready`.
- The image carries a header with `device_type`, `version`, `min_version` and an Ed25519 signature over the
  header and the image hash. The board checks device type, signature, and anti-downgrade before it
  reboots.
- After reboot the new firmware must pass its own self-check, join Wi-Fi, and answer a `POST /ota/confirm`
  from the host within a window (proposal 120 s); otherwise the bootloader rolls back
  (ESP-IDF app rollback). The host does the confirming, because only the host can tell the board is
  reachable from where it matters.
- The host side (SDK and a small script): `update_device(client, image_path)` uploads, waits for the
  reboot, checks `/identity` shows the new `firmware_version` and the same `serial`, then confirms.

## Signing

- Ed25519. One public key compiled into the firmware; the private key stays offline with the owner and
  signs a release with `scripts/sign-release.sh` after `scripts/build-release.sh`. A second public key
  slot is reserved for rotation.
- Development boards run development builds signed with a development key kept on the owner's machine.
  It is **not** the release key. A development-signed image installs only on boards flashed with the
  development public key.

## Flash layout: needs the owner

The board uses `SINGLE_APP_LARGE`: one app, no OTA slot. OTA needs two app slots and an `otadata`
partition, so the partition table changes, and that is a USB flash. On the development boards the
region from 0x200000 holds the factory MicroPython file system, which this project's rules say to keep.
Two choices:

1. **Two slots that fit before 0x200000**: each can be at most about 0.9 MB. The current app is
   983,600 bytes, so it does not fit; not possible without shrinking the firmware.
2. **Slots above the factory file system** (for example at 0x400000 and 0x800000 on a 16 MB part): the
   factory file system stays untouched; the partition table moves the app and adds slots. Needs one USB
   flash of the new bootloader/table/app and loses nothing already on the board.

Recommendation: 2. It needs the owner's okay, and a decision whether the first move to the new layout is
done on 6740 or 6718 first.

## Risks

- Nothing stops a stranger on the LAN from filling the inactive slot with garbage (denial of update, not
  takeover). Mitigation: a refused upload does not touch the running slot, and an upload is cancelled on
  timeout; add a rate limit on `/ota/upload`.
- A signed but buggy release reaches the boards the host updates; rollback saves only boards where the
  failure happens before the confirm. The host should update one board first and watch it.
- A power cut during an upload leaves the inactive slot half-written; the running slot is unaffected.
- The streaming handler is new server code: tested on the host with a fake flash, then on a board with a
  deliberately bad image, a truncated upload, and a cut connection.
- Writing the key to flash is the single high-value secret: the private key never leaves the owner's machine.

## Order of work (proposal, needs the owner's go)

1. Owner decides the flash layout (above) and whether the USB `ota` command (USB section, item 2) is wanted or esptool alone is enough.
2. Host-testable parts: image header parse, signature check, state machine, anti-downgrade; a local test
   server in Python that streams an image.
3. Streaming `POST /ota/upload` and rollback on the camera_board example, first on one board with a
   throw-away bad image.
4. SDK `update_device` and the sign script; rehearsal on the three boards: good update, wrong signature,
   truncated upload, firmware that never confirms, power cut mid-upload.
