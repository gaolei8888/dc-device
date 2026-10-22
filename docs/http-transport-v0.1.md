# HTTP Transport v0.1

HTTP is the first reference transport for dc-device because it is easy to inspect, debug, and use from DCO-Edge.

## Endpoints

### GET /health

Returns liveness and device identity.

### GET /manifest

Returns the Device Capability Manifest.

### GET /identity

Cheap device identification and the device-class profile reference. See
`identity-and-profile-v0.1.md`.

### GET /status, GET /diagnostics, POST /self_test

Health reporting. See `health-contract-v0.1.md`. `/health` remains a pure
liveness probe.

`POST /self_test` answers 409 `Busy` while a capability with
`safety.max_duration_ms` is running, because a self test can take seconds and
would delay the watchdog that must stop it (`safety-kernel-v0.1.md`). Stop the
capability, then retry. `/status` is not affected.

### POST /invoke

Request:

```json
{
  "capability": "servo.move",
  "arguments": {
    "angle": 30
  }
}
```

Response:

```json
{
  "ok": true,
  "capability": "servo.move",
  "result": {
    "angle": 30
  }
}
```

Errors: 400 `InvalidArguments` (arguments fail the manifest schema or the
handler), 404 unknown capability, 429 `RateLimited` (`safety.max_rate_hz`,
see `safety-semantics-v0.1.md`), 500 `DeviceError`.

## Design rule

Transport is not part of the capability semantics.

The same capability should work over HTTP, WebSocket, MQTT, serial, or another transport without changing its name or input schema.

## Security

### Authentication (access token)

Calls that act on the device or read its details need an access token:
`POST /invoke`, `POST /self_test`, `GET /diagnostics`. `GET /health`, `/identity`,
`/manifest` and `/status` are open and never look at the `Authorization` header: a wrong
token sent to them is answered normally and counts for nothing.

- The token is 32 random bytes written as 43 URL-safe base64 characters, sent as
  `Authorization: Bearer <token>`.
- It is obtained only over the USB cable (`token` command,
  `usb-setup-protocol-v0.1.md`). Someone who has only network access cannot get one.
  Pairing again with the cable replaces the token; the old one stops working at once.
- The device keeps a SHA-256 hash of the token and compares in constant time. The token
  never appears in any HTTP reply, `/diagnostics` or log.
- Missing or wrong token: `401`, header `WWW-Authenticate: Bearer`, body
  `{"ok":false,"error":"Unauthorized","message":"..."}`; nothing is executed. A release
  build with no token set answers `401` with error `Unpaired`; a development build with no
  token set allows the call.
- Ten wrong tokens in a row: every protected call answers `401 Unauthorized` for the next
  30 seconds, without looking at the token. A request with no header at all does not
  count. The lockout is global, so a hostile neighbour can slow the owner down; this is
  accepted. The counter is kept in memory only and is shown in `/diagnostics`.
- `GET /identity` carries `"auth":{"required":bool,"paired":bool}` so a host can see
  before it calls whether it must pair. Devices with no access control omit it.

Limits of this step: HTTP is still cleartext, so the token can be read off the local
network by someone on it and looking. It keeps out casual and unintended access, not a
determined attacker on the same Wi-Fi. Authentication says who may call a capability, not
whether the call is safe; `safety-semantics-v0.1.md` applies unchanged.

### Still to add before remote or production use

- authorization (roles, scopes)
- replay protection where appropriate
- TLS or a trusted local tunnel
- rate limits beyond the lockout
- audit logging
