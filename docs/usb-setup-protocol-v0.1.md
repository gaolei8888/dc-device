# USB setup protocol v0.1

Status: draft. Specification only; no firmware implements it yet. Nothing here
has been run on hardware.

Purpose: the first time a device is connected, a host application (DC Desktop)
gives it Wi-Fi credentials over the USB cable. After that the cable only
supplies power and all control goes over Wi-Fi using the HTTP transport
(`http-transport-v0.1.md`). This protocol is not part of the capability
contract and carries no capability invocations.

Scope: ESP-IDF (C/C++) firmware. MicroPython devices do not use it; see
"Not covered".

## Transport

- The device's console serial port: native USB Serial/JTAG or CDC on an S3,
  or a USB-to-serial chip on other boards.
- UTF-8 text, one message per line, lines end with `\n` (a `\r` before it is
  ignored). Maximum line length 512 bytes including the prefix. A longer line
  is discarded up to the next `\n`.
- The port is shared with the firmware's log output. Log lines and any other
  text are normal and must be tolerated.

## Framing: a fixed prefix

Every protocol line, in both directions, is:

```
DCSETUP <json>\n
```

The prefix is `DCSETUP` followed by exactly one space.

- **A line without the prefix is ignored by both sides.** The device never
  parses log noise and the host never parses boot output.
- The sender starts each protocol line with a line break (`\n`) of its own, so
  that the line begins at the start of a line even if a partial log line was
  already on the wire. Empty lines are ignored.
- Both sides ignore unknown JSON fields.

## Messages

Request (host to device):

```json
DCSETUP {"v":1,"id":7,"cmd":"hello"}
```

Success response (device to host):

```json
DCSETUP {"v":1,"id":7,"ok":true, ...}
```

Failure response:

```json
DCSETUP {"v":1,"id":7,"ok":false,"error":{"code":"wifi_auth_failed","message":"The Wi-Fi password was rejected."}}
```

- `v`: protocol version, an integer. This document is version 1.
- `id`: an integer chosen by the host, echoed in the response. A request the
  device cannot parse is answered with `"id":null`.
- `message` is for logs and developers. The host decides what a user sees
  from `code`.
- The device handles one request at a time and answers each exactly once.
- Unsolicited `event` lines may be sent while a command runs (see `wifi`).
  They have no `ok`:
  `DCSETUP {"v":1,"id":7,"event":"progress","stage":"connecting"}`.

### Errors that apply to any command

| `code` | Meaning |
|---|---|
| `bad_request` | Not valid JSON, or a required field is missing or has the wrong type. |
| `unsupported_version` | `v` is not supported. The response carries `"supported":[1]`. |
| `unknown_command` | `cmd` is not recognised. |
| `line_too_long` | The line exceeded 512 bytes. Sent with `"id":null`. |
| `busy` | A previous command is still running. |
| `timeout` | The device gave up on the command (see timeouts). |
| `internal` | The device failed for a reason that is not the host's fault. |

## Commands

### `hello`

Request: `{"v":1,"id":1,"cmd":"hello"}`

Response fields:

| Field | Meaning |
|---|---|
| `setup_protocol` | Highest protocol version the device speaks (integer). |
| `device_id`, `type`, `model`, `firmware_version` | As in `GET /identity`. |
| `serial` | The hardware serial, the MAC without separators in upper case. Same value as `/identity`. |
| `profile` | `{ "id", "version" }` if the device declares one. |
| `state` | `unconfigured`, `connecting`, `connected` or `offline` (see "Device states"). |
| `port` | The HTTP port, when `state` is `connected`. |
| `ip` | The IPv4 address, when `state` is `connected`. |

`hello` carries no secret. It is how a host recognises a device regardless of
which serial port name it currently has.

### `wifi`

Request:

```json
{"v":1,"id":2,"cmd":"wifi","ssid":"SchoolNet","password":"correct horse"}
```

- `ssid`: 1 to 32 bytes of UTF-8.
- `password`: empty for an open network, otherwise 8 to 63 characters
  (a WPA2/WPA3 passphrase). Raw 64-digit keys are not supported.

The device connects with the new credentials and answers when it has an
address or has failed. It may send `progress` events with `stage` set to
`connecting` or `getting_address`.

Success:

```json
{"v":1,"id":2,"ok":true,"state":"connected","ip":"192.168.1.20","port":8787}
```

Credentials are written to non-volatile storage **only after the connection
succeeded**. On failure the device keeps whatever it had before, and an old
working configuration is not destroyed by a typo.

Failure codes:

| `code` | Meaning |
|---|---|
| `invalid_ssid` | Empty or longer than 32 bytes. |
| `invalid_password` | Not empty and not 8 to 63 characters. |
| `wifi_not_found` | No access point with that name was seen. |
| `wifi_auth_failed` | The access point rejected the password or security mode. |
| `wifi_no_address` | Associated, but no address was obtained (DHCP). |
| `wifi_timeout` | None of the above within the time limit. |

### `forget_wifi`

Request: `{"v":1,"id":3,"cmd":"forget_wifi"}`. The device erases its stored
credentials, stops the HTTP server and moves to `unconfigured`. Response:
`{"ok":true,"state":"unconfigured"}`. This lets a school reuse a board.

### `token`

**Pending gap 15.** The shape below is a placeholder and must not be
implemented until the authentication scheme is decided (header name, scope,
revocation, whether `/identity` stays unauthenticated).

Request: `{"v":1,"id":4,"cmd":"token"}`. The device generates a new access
token, stores it, replaces any previous one (which stops working) and returns
it once: `{"ok":true,"token":"..."}`. Receiving a token over the cable is the
physical-presence pairing step for gap 16. It is not a button press, but it
requires holding the device.

Note that v0.1 HTTP is cleartext, so the token can be read off the local
network. It keeps out casual access, not an attacker on the same network.

## Timeouts

The device always answers, so the host can tell "failed" from "silent".

| Command | Device gives up after | Host should wait at most |
|---|---|---|
| `hello`, `forget_wifi`, `token` | 2 s | 4 s |
| `wifi` | 20 s | 25 s |

When the device gives up it answers `timeout`. If the host gets nothing within
its limit, the device is not speaking this protocol (wrong port, not booted,
or the chip is in download mode); see "Host behaviour".

## Secrets

- The password appears only in the `wifi` request. It is never echoed in a
  response, an event or an error message.
- The firmware must not log protocol lines or the SSID-and-password pair, at
  any log level, and must not include them in `/diagnostics`.
- The host must not write the password to its logs or keep it after the
  command (a keychain, if the user asks to remember it, is the only exception).
- At rest the credentials are in NVS in plaintext unless flash encryption is
  enabled, which this version does not require. Anyone with the board can read
  them.

## Device states

| `state` | HTTP server | Meaning |
|---|---|---|
| `unconfigured` | not running | No stored credentials. Only this protocol is served. |
| `connecting` | not running | Credentials stored, joining the network. |
| `connected` | running | Has an address. |
| `offline` | not running | Credentials stored but the network cannot be joined. |

Rules for the firmware:

- **Never block on Wi-Fi.** The setup reader runs in its own task and answers
  within its timeouts in every state. A failing network must not stop it from
  answering `hello`.
- On a failed connection, move to `offline`, keep serving this protocol, and
  retry with a growing delay (5 s doubling to a maximum of 60 s). Do not
  reboot in a loop.
- This protocol stays active after setup, so a host can read the current `ip`
  with `hello` later. It never replaces the HTTP transport.
- A device that declares a `motion` capability asserts its safe state at boot
  whatever the Wi-Fi state (`safety-kernel-v0.1.md`). Setup and safety are
  independent.

## Host behaviour (non-normative)

How the host recognises the device and what it tells the user is the host's
decision. These points come from how ESP32 USB behaves.

- **The port name can change.** On a chip with native USB, reset, flashing and
  entering or leaving the ROM download mode make the port disappear and come
  back, often under a different name. The host must not remember a port name
  across a reset. After flashing or rebooting, wait up to 10 s for a port to
  appear, then identify the device with `hello` and compare `serial` with the
  MAC read from the chip during flashing.
- **Log text is normal.** After a reset the port first prints boot output.
  Send `hello` only after the output pauses, and retry it every second a few
  times before giving up.
- **When the BOOT key is needed.** Automatic entry into download mode works
  through the USB Serial/JTAG controller or the DTR and RTS lines of a
  USB-to-serial chip. It fails when the firmware itself uses TinyUSB for the
  port and on boards without the auto-reset circuit. The host can conclude this
  when (1) a serial port is present, (2) three `hello` attempts get no answer,
  and (3) the ROM flashing handshake also fails. Only then should it ask the
  user to hold BOOT, press and release RESET (or unplug and replug the cable
  while holding BOOT), release BOOT, and try again. It should not ask for this
  earlier, because most boards do not need it.
- **The board used for development is not an S3 native USB board.** It
  reports a WCH CH343 (vendor 0x1A86, product 0x55D3, "USB Single Serial"),
  seen on macOS as a `usbmodem` port. It relies on the CH343's DTR and RTS lines
  for automatic reset, which works only if the board has the auto-reset
  circuit. This was read from the system USB listing by another session, not
  tested here, and whether Windows needs a driver for it is not known.
- **Do not assume a driver.** Chips that appear as a plain serial device
  without a vendor driver are preferable for school machines without
  administrator rights. This is a hardware choice, not part of the protocol.

## Open items

- `token`: waits for gap 15 and, for the pairing meaning, gap 16.
- Where firmware images are distributed from and who builds them with ESP-IDF
  are undecided (pinned version and checksum, preferably shipped with the host
  installer).
- Flash and NVS layout (namespace and keys for the credentials).
- Whether the host should be able to read the current state without opening the
  port exclusively (for example while another tool holds it).
- Behavior when two hosts talk at once: not defined; assume one.

## Not covered

- **Flashing.** Writing the firmware image through the ROM bootloader is the
  host's job (it is the esptool protocol) and not part of this protocol.
- **MicroPython devices.** Decided with the user on 2026-10-12: school
  MicroPython boards are a supported route, only for devices that do not move
  (sensors, lights, displays), at lower priority. Such a board keeps MicroPython
  and runs a dc-device program that speaks the HTTP contract; the host places
  the files and writes the Wi-Fi configuration through the MicroPython raw
  REPL. This setup protocol does not apply to them. Devices that move (cars,
  arms) always get C firmware, because a garbage-collected runtime gives weaker
  timing for the safe state (`safety-kernel-v0.1.md`).
- **Access to the device after setup**: the HTTP transport and, when decided,
  its authentication (gaps 15 and 16).
- **Discovery** (gap 17). After setup the host already knows the address from
  the `wifi` response.
