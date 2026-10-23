# HTTP Transport v0.1

HTTP is the first reference transport for dc-device because it is easy to inspect, debug, and use from DCO-Edge.

## Endpoints

### GET /health

Returns liveness and device identity.

### GET /manifest

Returns the Device Capability Manifest: the capabilities declared right now. A device may
leave out a capability whose part it has no evidence for, and the list can change while the
device runs (see `self-check-v0.1.md`, "Declaring capabilities"); a host re-reads it after a
self-test or after it declared a part. `POST /invoke` of a capability that is not declared
answers 404 `UnknownCapability`.

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

An error body is `{"ok":false,"error":"<name>","message":"<text for people>"}`. A handler may add
a stable, machine-readable `code` between them:
`{"ok":false,"error":"DeviceError","code":"camera_no_frame","message":"..."}`. A host branches on
`code` when it is present and ignores it when it does not know it. `code` is additive: a device
that never sets one is unchanged. Codes in use are listed with the capability that raises them
(`superpowers/specs/2026-10-22-camera-capture-and-screen-block-contract.md`).

### Expected duration

A capability in `/manifest` may carry `"expected_duration_ms":{"typical":N,"max":M}` (`typical`
optional). `max` is how long a host waits before calling the result unknown, plus its own network
margin; it is information, not a limit, and not `safety.max_duration_ms`. Without the field the host
uses its own default. A timeout does not prove the action did not happen: read state again before
deciding.

### Request size

A POST body larger than the device's limit is refused with 413. The default limit is 2048 bytes;
the camera_board example raises it to 12288 so that one `screen.write_block` (at most 7680 pixel
bytes, about 10.3 KB as base64) fits.

## Design rule

Transport is not part of the capability semantics.

The same capability should work over HTTP, WebSocket, MQTT, serial, or another transport without changing its name or input schema.

## Security

v0.1 is for local development only.

Before remote or production use, the transport must add:

- device identity
- authentication
- authorization
- replay protection where appropriate
- TLS or a trusted local tunnel
- rate limits
- audit logging
