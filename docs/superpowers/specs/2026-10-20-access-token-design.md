# Access token (authentication) for dc-device - design

> **DROPPED by the owner on 2026-10-22 ("不要访问码").** Kept for the record only; the code is on the unmerged branch `feature/access-token` and must not be merged. Do not build from this.


Status: draft for approval, 2026-10-20. Nothing here is built.
Closes gap 15 of `docs/contract-gaps-draft.md` (authentication) and gives the `token`
command of `docs/usb-setup-protocol-v0.1.md` its meaning. It is the first step of
`docs/superpowers/specs/2026-10-20-ota-design.md`, which needs it for its settings.

## Intent

Today anyone on the same network can call `POST /invoke` on a board: make it move,
beep, or (soon) change its update settings. v0.1 said so openly ("local development
only", `docs/http-transport-v0.1.md`). Boards are about to go to customers, so a board
must only obey a host it was paired with.

## Goals

1. Calls that act on the device or read its details need an access token. A call without
   a valid one is refused and does nothing.
2. A token is obtained by holding the board: over the USB cable, the same way Wi-Fi is
   set (`token` command). Someone who only has network access cannot obtain one.
3. A customer who loses the token, or changes computer, pairs again with the cable. The
   old token stops working at once.
4. Release firmware with no token set refuses the protected calls (locked until paired).
   Development builds may stay open when unpaired, so bench work and the existing
   test scripts keep working.
5. The Linux reference device and the simulator speak the same scheme, so one host
   implementation works against all.

## Non-goals

- Encryption. HTTP stays cleartext in this step: the token can be read off the local
  network by someone who is on it and looking. The token keeps out casual and
  unintended access, not a determined attacker on the same Wi-Fi. HTTPS on the board
  (a certificate whose fingerprint is handed over the cable together with the token) is
  the next step and is not designed here.
- Several tokens, scopes, roles, expiry, or telling hosts apart. There is one token per
  board.
- Hiding `/identity` (it exposes the MAC as `serial`); hosts need it to find boards.
- Any change to the OTA design beyond the dependency.

## Anti-goals (failure even if it works)

- A board that can be taken over from the network by a call that skips the check.
- A token that appears in logs, `/diagnostics`, `/status`, or any reply other than the
  single USB reply that issues it.
- A guessable token.
- A locked board with no way back: the customer must always be able to pair again by
  cable, whatever state the board is in.

## The scheme

### What is protected

| Endpoint | Token needed |
|---|---|
| `GET /health`, `GET /identity`, `GET /manifest`, `GET /status` | no |
| `POST /invoke`, `POST /self_test`, `GET /diagnostics` | yes |
| OTA calls that change settings or start an update (OTA design) | yes |
| OTA read-only `GET /ota/status` | no |

Reasoning: the unprotected ones are what a host needs to find a board and see that it
is alive. `/diagnostics` lists pins and buses, so it is protected. Whether `/manifest`
should be protected is the owner's to veto; the default is open because a host reads it
before it has any reason to pair.

### The token

- 32 random bytes from the hardware random generator, written as 43 characters of
  URL-safe base64. Long enough that guessing is not a concern.
- Sent as `Authorization: Bearer <token>`.
- The board stores only a SHA-256 hash of it (NVS namespace `dcauth`), and compares in
  constant time. The token itself exists in one place on the board's side: the USB reply
  that issued it.

### Replies

- Missing or wrong token: `401`, `WWW-Authenticate: Bearer`, body
  `{"error":{"code":"Unauthorized","message":"..."}}`. Nothing is executed. The reply
  for a wrong token and for a missing one is the same.
- Release board with no token set: `401`, code `Unpaired`, message tells the host to pair
  by cable. Development board with no token set: the call is allowed, and
  `/identity` says so (below).
- Failed attempts: after 10 failures in a row the board answers `401` to all protected
  calls for the next 30 s without looking at the token (so guessing cannot be fast), and
  counts failures in a counter that `/status` does not show but `/diagnostics` does. A
  correct token resets the run. The lockout is global, not per client, so a hostile
  neighbour can slow a legitimate host; that is accepted and noted.

### Telling the host which state the board is in

`GET /identity` gains `"auth": {"required": true|false, "paired": true|false}`. A host
learns before it calls that it must pair, without trying a protected call. (An additive
field; no `spec_version` change beyond what the contract's compatibility rules already
allow - the builder checks them.)

### USB setup protocol (replaces the placeholder in that document)

- `token`: request `{"v":1,"id":N,"cmd":"token"}`. The board generates a token, stores
  its hash (replacing any earlier one, which stops working), and answers
  `{"v":1,"id":N,"ok":true,"token":"..."}` exactly once. The reply is printed on the
  console port; whoever can read that port holds the board, which is the point. It is
  never logged and never repeated. Timeout 2 s, error `internal` if the store fails.
- `unpair`: request `{"v":1,"id":N,"cmd":"unpair"}`. Erases the stored hash; the board
  returns to "no token set". For a board that is sold, returned or reused.
- `hello` gains `"paired": true|false` (additive).
- `forget_wifi` does not touch the token.

### Where the check lives

In the endpoint code (`Endpoint::handle`), not in one transport, so every transport gets
it. The HTTP server passes the `Authorization` header through. The Python SDK
(`Device` / `http_server.py`) gets the same check with the token supplied in its
constructor, and the simulator (`dc-device-sim`) accepts a token for fault-free
parity.

### What the host (DC Desktop) must do - not built here

Keep the token in the operating system's secret store, send the header on every call,
and on `401` offer to pair again by cable. This repo gives DC the exact contract
and nothing else; DC's own work is DC's.

## Details settled after review

- **Release vs development** is the existing build flag `CONFIG_DC_DEVICE_RELEASE`
  (release builds have no `debug.*` capabilities and no compiled-in network). A
  development board that has been paired enforces the token like any other; only an
  unpaired development board is open. Firmware installed by OTA is always a release
  build (the signing script refuses to sign anything else), so an update cannot turn a
  customer's board into an open one.
- **Re-keying by cable is by design.** `token` on an already paired board replaces the
  token without asking: whoever holds the board and a cable owns it, the same as for
  Wi-Fi settings. No button press is added. A malicious program on the computer that
  holds the cable could already read the host's stored token, so a button would not
  change what it can do.
- **Linux device and simulator** have no cable. The token is a constructor argument (the
  SDK) or a command-line option / environment variable (the simulator); there is no
  pairing endpoint. Tests use a fixed token.
- **The failure counter lives in RAM.** A reboot clears it; flash is not written for
  failed attempts.
- **Only HTTP** calls `Endpoint::handle` today. `handle` takes the `Authorization`
  header value as an input (empty = none). A future transport must supply credentials or
  be refused; it is never trusted by default. The USB setup channel does not go through
  `handle` and is physical access by definition.
- **Contract compatibility:** `schemas/device-identity.schema.json` allows additional
  properties at the top level, so `auth` is an additive field and the existing schema
  check and fixtures keep passing; the schema file gets the new field described.
- **`/manifest` stays open** (my call; the owner may veto).
- **Open endpoints ignore the header.** A host may send `Authorization` on every request
  (DC does). On `/health`, `/identity`, `/manifest`, `/status` the header is not looked
  at: a wrong token there is answered 200 and does not count toward the 10 failures.
  Only the protected endpoints check and count.

## Safety link

Authentication decides who may call a capability. It says nothing about whether the
call is safe; the safety rules (`docs/safety-semantics-v0.1.md`) apply unchanged, and
the watchdog still ends a time-limited action after its limit regardless of who started
it or whether they are still authorised.

## Order of work

1. Contract text: `http-transport-v0.1.md` security section, `identity` field,
   `contract-gaps-draft.md` gap 15 and gap 16 status, `usb-setup-protocol` commands.
2. Endpoint check and the token store, with host tests (missing, wrong, right,
   lockout, unpaired in both build modes, no token in any output, constant-time
   compare used).
3. USB `token`, `unpair`, `hello.paired` in the setup protocol, host-tested.
4. Python SDK and simulator.
5. Board test on a development board: pair, call, wrong token, re-pair (old stops),
   unpair, power cycle keeps the pairing, token never visible in any HTTP reply or log.

## Decided by the owner

- A token is obtained over the USB cable. Losing it means pairing again by cable.
- OTA waits for this.

## My calls (change them if wrong)

- Release boards refuse protected calls until paired; development builds stay open.
- One token per board; no scopes or expiry.
- 43-character URL-safe token, hash stored, constant-time compare.
- `/diagnostics` protected; `/manifest` and `/status` open.
- 10 failures in a row -> 30 s global lockout.
- `unpair` command added.
- HTTPS is out of this step.

## Left to the builder

Header parsing details, the random source API, NVS key names, the constant-time compare
implementation, exact message texts, how the setup protocol tests are extended.
