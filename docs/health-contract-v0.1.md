# Health Contract v0.1

`dc-device` answers one question: **is this device healthy, and what exactly is wrong?**

It does not decide whether the whole system may proceed. dc-edge reads these reports and decides READY / DEGRADED / BLOCKED.

## Operations

| Operation | HTTP | Side effects | Returns |
|---|---|---|---|
| liveness | `GET /health` | none | `{"status":"ok","device_id":...}` (unchanged) |
| `get_status()` | `GET /status` | none, cheap | health report |
| `self_test()` | `POST /self_test` | may actuate hardware | health report |
| `diagnostics()` | `GET /diagnostics` | none | diagnostics |

`/health` only proves the device answers. It stays a liveness probe and is
not the health report. A device that is reachable but broken still answers
`/health` with `ok`; check `/status` for the real answer.

Schemas: `schemas/device-health.schema.json`, `schemas/device-diagnostics.schema.json`.

## Health report

```json
{
  "device_id": "camera_01",
  "status": "degraded",
  "checks": {"connection": "ok", "fps": "warning"},
  "issues": [
    {"code": "LOW_FPS", "severity": "warning", "check": "fps",
     "message": "fps below expected threshold"}
  ]
}
```

- Check outcomes: `ok`, `warning`, `failed`, `skipped`.
- `status` is derived, never set freely: any `failed` check gives `failed`,
  otherwise any `warning` gives `degraded`, otherwise `ok`.
- Every `warning` or `failed` check produces one issue (`warning` and `error`
  severity respectively). `skipped` and `ok` produce none.
- A device with no registered checks reports `ok` with empty `checks`.

`GET /status` runs only the cheap, read-only checks. `POST /self_test` runs
those plus the self-test checks, which are allowed to move things (blink the
LED, sweep the servo). Self-tests are serialized: a second request while one
is running waits for it.

The HTTP status is `200` whenever the device could produce a report, including
`failed` reports. A failing check is data, not a transport error. A check that
raises is reported as `failed` with code `CHECK_ERROR`, it never crashes the
endpoint.

## Diagnostics

```json
{
  "device_id": "camera_01",
  "status": "ok",
  "uptime_s": 1234.5,
  "capability_count": 3,
  "last_self_test": null,
  "info": {"firmware": "0.1.0", "free_heap": 183000}
}
```

`diagnostics()` is read-only and never runs a self-test. `last_self_test` is
the most recent report or `null`. `info` is free-form device-specific detail.

## Simulator compatibility

dc-device-sim must serve the same routes and shapes, with the same check
names and issue codes, so dc-edge's one-click readiness flow works unchanged
against real and virtual devices.
