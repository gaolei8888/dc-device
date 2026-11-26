# OTA firmware update for dc-device ESP32 boards - design

Status: draft for approval, 2026-10-20. Nothing here is built.

## Intent

Boards will be in customers' hands, out of reach. Today a new firmware needs a USB
cable and a computer (`docs/usb-setup-protocol-v0.1.md`, `scripts/build-release.sh`).
We want a board to fetch and install a new firmware by itself, over the customer's
Wi-Fi, from a service run by us. The owner's words: automatic, with a switch the
customer can turn off; any time of day.

The reason it is worth the work: a fleet nobody can reach is a fleet nobody can fix.
The reason it is dangerous: automatic means one bad release reaches every board at
once, and the boards cannot be touched afterwards. Everything below follows from those
two sentences.

## Goals

1. A board checks the cloud for a newer firmware, downloads it into the inactive
   slot, verifies it, reboots into it, and keeps it only if it proves it works.
2. A bad firmware never leaves a board unreachable: it rolls back by itself.
3. Only firmware signed by us installs. A hacked server, a hacked router or a
   hacker on the customer's Wi-Fi cannot push code.
4. We can release to a few boards first, widen it, and stop it, from the existing
   back office.
5. The customer can switch automatic updates off, in DC, and a board with the switch
   off never contacts the cloud for firmware.
6. An update never interrupts something dangerous: no install while a time-limited
   capability (motor, servo, ...) is running.

## Non-goals

- Updating the bootloader or the partition table over the air. They change only by USB.
- Delta (patch) updates. The whole app image is downloaded each time (about 1 MB).
- Secure boot and flash encryption. Signed images protect updates; a person with the
  board in hand and a USB cable is outside this design. Revisit before shipping to
  hostile environments.
- Pushing from the cloud to a board. Boards are behind customers' routers; the board
  always initiates.
- Updating Linux reference devices. They update as Python packages.
- Anything about the camera or other capabilities.

## Anti-goals (failure even if it "works")

- An unsigned or wrongly signed image gets installed.
- A board that cannot reach the cloud, or runs a firmware that cannot reach the cloud,
  stays on a broken version with no way back.
- An update starts while a motor is moving.
- The switch is off and the board still downloads firmware.
- The only copy of the signing private key sits on a server we run.

## What exists today

- `dc_classeditor` already has an OTA back office for a different firmware family
  (xiaozhi-esp32, school devices): `Esp32OtaController` (`GET /esp/ota`, public),
  `Esp32FirmwareController` (upload, list, set latest, sha256, size, `boardVariant`),
  domain classes `Esp32Firmware` and `Esp32Device` in `dc-common`
  (`currentVersion`, `lastOtaCheckAt`, per-tenant). The owner says nothing in the field
  uses `/esp/ota`; we leave it untouched anyway.
- This repo: single-app partition table (`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`),
  one 0x177000-byte app slot at 0x10000, NVS at 0x9000. No rollback support, no TLS
  client code, no `token` (HTTP has no authentication - `docs/http-transport-v0.1.md`).
- The release build is reproducible and writes `manifest.json` with sha256 per file.
  App image is about 0.97 MB.
- The boards have 16 MB flash. Their factory MicroPython file system starts at 0x200000.

## Parts

### 1. Flash layout (firmware)

Custom partition table, two app slots, OTA data:

| partition | offset | size |
|---|---|---|
| nvs | 0x9000 | 0x6000 |
| otadata | 0xF000 | 0x2000 |
| phy_init | 0x11000 | 0x1000 |
| ota_0 | 0x20000 | 0x1E0000 |
| ota_1 | 0x200000 | 0x1E0000 |

ota_1 overwrites the start of the factory MicroPython file system at 0x200000. This is
the builder's call below; the consequence is that the factory program on a board is
destroyed by the first USB flash of an OTA firmware. Boards already backed up:
`~/esp32-backups/`. For customer boards the factory program is not needed.
Bootloader app rollback is enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
This layout change itself ships only by USB; the first OTA-capable release is flashed
at 0x0, 0x8000 and the first slot.

### 2. Signing

- Ed25519. Private key held offline by the owner, never in the repo, never on a server.
  Public key compiled into the firmware (one key; a second "next" key slot is reserved
  so a key can be rotated by an update signed with the old one - the rotation procedure
  is left to the builder).
- A detached signature over a small signed record: `{model, version, size, sha256,
  min_version}`. The board verifies the record, then checks the downloaded image against
  `size` and `sha256` while streaming it into the slot. Nothing is booted before both
  match.
- `min_version`: the oldest firmware version allowed to install this one directly. A board
  below it refuses the image; the back office serves a board the newest release whose
  `min_version` is not above the board's version, so an incompatible jump is done in
  steps.
- Anti-downgrade: a board refuses a `version` lower than its own unless the signed
  record carries `allow_downgrade: true`. (How to roll a bad release back for boards
  that already installed it is a release procedure: publish a higher version that
  contains the old code.)
- `scripts/sign-release.sh` (offline, run on the owner's machine) takes the release
  directory and the private key path and adds the signed record next to `manifest.json`.

### 3. Board behaviour (firmware)

- A task `dc_ota`, low priority, own stack. Never runs inside the HTTP handler or the
  `dc_tick` watchdog task.
- State in NVS namespace `dcota`: `auto` (default 1), `last_check`, `last_result`,
  `pending_version`.
- Check: when auto is on, after Wi-Fi is up, then every 24 h with a random offset
  (so a fleet does not call at once), and on demand. HTTPS to the service URL (compiled in as the production default; a staging build or a
  board can be pointed elsewhere by the `provision` command, stored in NVS), certificate verified against the ESP-IDF certificate bundle. The board needs the
  correct time for this (SNTP; certificate checks fail without it).
- The request: `serial`, `model`, `firmware_version`, `slot`, `auto`. The reply is
  either "no update" or the signed record, the download URL and a per-request expiry.
- Install conditions: auto is on (or the install was requested by hand), no
  time-limited capability is running (`Endpoint::time_limited_running()` is false), no
  self test is running. If a condition fails, try again in 10 minutes. A manual
  request with auto off still works - the switch stops unattended updates, not the
  customer's own.
- Download into the inactive slot with `esp_ota_*`, checking sha256 as it streams.
  A failure (network, size, hash, signature) leaves the running slot untouched, is
  recorded in `last_result`, and is retried at the next check with backoff.
- After a good write: set the new slot as boot slot in the "pending verify" state and
  reboot.
- First boot of a new slot: the firmware must bring up Wi-Fi, start the HTTP server
  and answer its own `/status` within 120 s (it checks itself, the same way the
  hardware self test checks the board), and complete one check-in to the service
  (DNS, TCP, TLS with certificate verification, HMAC accepted). Only then it calls the
  rollback-cancel function. One exception, so a customer whose Wi-Fi has no internet
  does not roll back every good update: if the service cannot be reached at all (DNS or
  TCP fails) the check-in is skipped; any failure after the connection is made (TLS,
  certificate, rejected reply) counts as a failure. This is what stops a build that
  cannot update itself ever again from being kept. If
  that does not happen - crash, hang, no network - the bootloader returns to the old
  slot on the next reset, and a watchdog timer forces that reset at 120 s. The old
  version is then marked "failed" in NVS so the board does not install the same bad
  version again; it tries again only when the service offers a newer version.
- If the board's own Wi-Fi settings are gone or wrong, an update cannot fix it; that
  is not an OTA problem.

### 4. Board HTTP and USB interface (firmware)

- `GET /ota/status`: `{auto, state, current, available, last_check, last_result}`,
  with `state` one of idle, checking, downloading, waiting (a condition failed),
  rebooting.
- `PUT /ota/settings` `{auto: bool}` and `POST /ota/check` (check and install now).
- These two change the device and need the access code (`token`). Until `token`
  exists, they must not be shipped; `GET /ota/status` is read-only and may be open.
- `/identity`'s `firmware_version` stays the single place a version is read.
- The USB setup protocol gets no new command in this design except provisioning
  (below).

### 5. Back office (dc_classeditor)

New controller and routes under a new prefix (suggested `/dcdevice/ota/...`); the
existing `/esp/ota` is not changed. Reuses `Esp32Firmware` and `Esp32Device` where they
fit; fields the owner must add by hand-written migration (this design does not generate
migration files):

- firmware: `signedRecord` (text), `signature`, `minVersion`, `model`, `rolloutPercent` (0-100), `halted` (boolean), `sizeBytes` already
  exists.
- device: `autoUpdate` (as last reported),
  `lastOtaResult`, `lastOtaAt`, `targetVersion` (exists in the domain, reused as a
  per-device pin).
- Selection for a board: the newest non-halted firmware for its model whose
  `rolloutPercent` covers hash(serial) mod 100, unless the device has a pinned target.
- Controls: upload a signed release, set percent, halt (stops handing out that
  firmware; boards that already have it keep it), see how many boards run each version
  and who failed.
- Download URLs are short-lived signed links to the existing COS storage; the board
  does not need a login for them, because the signature, not the link, is the trust.

### 6. Who is the board? (provisioning)

The owner approved: each board has its own key, written at flashing time. The check
request carries an HMAC of `serial|timestamp` with that key, so the back office can tell
a registered board from a guess and can count and steer the fleet. It is not what
protects the firmware (the signature is).

The back office must be able to recompute the HMAC, so it cannot keep only a hash of the
key. Decision (my call): the back office holds one master secret in its server
configuration and derives each board's key as `HMAC(master, serial)`. It stores nothing
per board for this. Consequence: whoever gets the master secret can forge any board's
check-in (statistics and rollout steering), but still cannot get a board to install
anything, because that needs our signature. The `deviceKeyHash` field is not needed.

Flow, to be confirmed against DC: when DC flashes a board it asks the back office for
that serial's key, and writes it to the board over USB with a new setup command
`provision` (accepted only while no key is stored; it can also set the service URL, see
section 3). A board with no key checks in anonymously, is still served by the same
selection rule, and cannot be pinned to a version.

### 7. Release procedure

`scripts/build-release.sh` -> `scripts/sign-release.sh` (offline) -> upload in the back
office -> set canary at a small percent -> watch failures -> widen -> 100%. A halted
release is replaced by publishing a higher version.

## Order of work

1. Access code (`token`) - its own design; OTA settings depend on it.
2. Flash layout, rollback and the on-board update path against a local test server
   (a Python server in this repo); host tests for the signed-record check and the
   state machine; board tests including a deliberately bad image.
3. Back office.
4. Provisioning and the DC flashing flow.
5. A real fleet rehearsal on the three development boards: good update, corrupted
   download, wrong signature, bad firmware that never confirms, power cut mid-download,
   switch off.

## Decided by the owner

- OTA is released only after the access code (`token`) exists. The customer's off switch
  goes through an authenticated board setting; there is no USB-only stopgap.
- Automatic by default, customer can turn it off.
- Any time of day.
- The back office lives in `dc_classeditor`.
- Per-board key written at flashing time.
- The service is ours and boards pull; nothing is pushed.

## My calls (change them if wrong)

- Ed25519, offline key, one compiled-in public key plus a reserved second.
- Two 1.9 MB slots at 0x20000 and 0x200000, destroying the factory file system.
- 24 h check with random offset; 10 min retry when a condition blocks; 120 s to prove a
  new firmware works.
- Certificate bundle from ESP-IDF rather than pinning one certificate.
- Rollout by hash of serial and version; no separate canary channel.
- The back office derives board keys from one master secret; stores nothing per board.
- A new slot is kept only after one good check-in to the service, unless the service is unreachable altogether.
- `min_version` makes incompatible jumps go in steps.
- Service URL compiled in, overridable by `provision`.
- New routes, existing `/esp/ota` untouched.
- Anti-downgrade on by default.

## Left to the builder

Library choices (esp_https_ota vs. hand-written streaming, the Ed25519 implementation),
exact NVS key names, the key rotation procedure, test server details, field types.
