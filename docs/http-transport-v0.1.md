# HTTP Transport v0.1

HTTP is the first reference transport for dc-device because it is easy to inspect, debug, and use from DCO-Edge.

## Endpoints

### GET /health

Returns liveness and device identity.

### GET /manifest

Returns the Device Capability Manifest.

### GET /status, GET /diagnostics, POST /self_test

Health reporting. See `health-contract-v0.1.md`. `/health` remains a pure
liveness probe.

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
