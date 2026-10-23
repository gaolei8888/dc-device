# P0 device evidence (hello -> identity -> manifest -> heap.read)

Status: draft, 2026-10-22. Written for `docs/2026-10-22-hw-bench-integration-action-plan.md`
(P0-A, P0-B, P0-C). It states what the device side provides as evidence and what it does
**not** yet evidence. Approval, USB enumeration and the decision to let an AI act are the
host's (DCD), not this repo's.

## 1. What the device says, and where

| Step | Source | Fields that matter | Note |
|---|---|---|---|
| USB reply | `DCSETUP hello` (UART console, `usb-setup-protocol-v0.1.md`) | `serial`, `device_id`, `type`, `model`, `firmware_version`, `setup_protocol`, `state`, `ip`, `port` | answers in every network state |
| Network identity | `GET /identity` | the same names, plus `name` | answers even if `/status` is failed |
| Capabilities | `GET /manifest` | `capabilities[]` | only what the firmware has evidence for (self-check saw it, or a person declared it over USB); can change while the board runs |
| Health | `GET /status` | `status`, `checks` | `/health` only says the HTTP service runs |
| First action | `POST /invoke` `heap.read` | `result.free_heap_bytes` | read-only, no motion, safe to repeat |

`serial` is the chip's base MAC (`esp_efuse_mac_get_default`, the same address as the Wi-Fi
station), written as 12 hexadecimal digits in upper case without separators, for example
`AABBCCDDEEFF`. It is the same string in `hello` and `/identity` (one variable fills both).
`device_id` (`camera_board_01`) is the same on every board of that example and does **not**
tell boards apart. The serial number of the USB-to-serial chip (CH34x) is a different thing
and says nothing about the board behind it.

## 2. The matching rule (USB <-> network)

Reference implementation: `sdk/python/dc_device/identity_match.py`
(`normalize_serial`, `serials_match`, `identity_matches_hello`), tests in
`tests/python/test_identity_match.py`.

- Normalize: remove `:` `-` `.` and whitespace, upper-case. The result must be exactly 12
  hexadecimal digits, else the serial is **invalid** (not equal to anything, not even to
  another invalid serial).
- Match: both serials valid and equal after normalizing. A missing serial on either side is
  a mismatch.
- Also compare `device_id`, `type` and `firmware_version` when both sides have them. A
  firmware version that differs between `hello` and `/identity` means the board was
  re-flashed or two boards are being confused: mismatch.
- On any mismatch the host must not link the two or reuse an earlier approval; it shows
  which fields differ. It must not retry its way to a match.

## 3. Evidence states

The device can provide evidence for: `SETUP_RESPONSIVE` (a `hello` reply), `NETWORK_REACHABLE`
(`/health` answers), `IDENTITY_VERIFIED` (the rule above passes), `CAPABILITIES_VERIFIED`
(`/manifest` read; for a specific part, its self-test result). `USB_PRESENT` and `AI_ALLOWED`
are the host's: the device never decides either. A manifest entry whose part cannot be
observed is "declared", not "verified".

## 4. Examples (no credentials)

Real captures, with the MAC replaced, are in `tests/fixtures/p0/` (see its `README.md`),
taken from a board running `main` (`0.1.0+deae116`): `identity.json`, `manifest.json`,
`status.json`, `health.json`, `invoke_heap_read.json` and three error cases (unknown capability,
a capability the board has no evidence for, malformed body). `hello.json` there is **built from
the host test, not captured** from a board.

Errors seen on `/invoke` (HTTP status, `error`): `404 UnknownCapability` (also for a capability
the board does not declare), `400 InvalidRequest` (malformed body), `400 InvalidArguments`,
`409 Busy`, `429 RateLimited` (the 429 reply is sent as `500` by the current firmware's status
line; a known defect), `500 DeviceError`.

## 5. Failure, timeout and retry

- `heap.read` is idempotent: a host may repeat it after a failure or timeout.
- For any capability with `kind: "action"`, an HTTP timeout or a lost connection means the
  result is **unknown**: the request may have been received and acted on. A host must not
  repeat it automatically; it reads state again (for example `/status`, or the capability's own
  read) and asks the user.
- A reply to `/invoke` says the call was handled, not that a physical effect finished; actions
  that take time say so in their description, and the manifest has no expected-duration field yet.
- Offline: if the network is down, `hello` still answers (it needs no network).

## 6. What is not evidenced yet

Not done, so not claimed:

1. **Tested combination.** The exact board model, flash and PSRAM sizes, USB-serial bridge and
   board revision for 6740 and 6718 are not recorded here. This needs the owner's input, not a guess.
2. **Hello timing on a board.** Opening the console port resets these boards; the documented
   rule is to wait about 2 to 2.5 s before the first command. A measured table (reset to first
   `hello` reply, by network state, with timestamps) has not been recorded.
3. **Hello without Wi-Fi on a board.** Host tests cover all four network states; a dedicated
   recorded run on a board with the network unavailable is not on file.
4. **A real `hello` capture.** It needs a board's serial port to be opened, which resets the board.
   It waits for the owner's approval.
5. **No authentication.** The owner decided against an access token (2026-10-22). Every call,
   including `/invoke`, is open to anyone on the same network. The code for a token exists on
   the unmerged branch `feature/access-token`.
6. **`/status` can say ok while a part is unverified.** On 6740 the `peripherals` check reports
   `ok` although `/diagnostics` lists the microphone, speaker and screen as `unverified`.
   Those parts are not declared in `/manifest`, so nothing offered to an AI depends on them;
   a host that wants the detail reads the `/diagnostics` `peripherals` rows, not only `/status`.
7. **The camera is detected but cannot capture.** `/diagnostics` shows the sensor found; there is
   no capture capability, and the driver has never delivered a frame on this board.
