# ESP32-S3 Robot Car Reference Device

This is the first real-hardware reference implementation for dc-device.

## Current capabilities

- `GET /health`
- `GET /manifest`
- `GET /status`, `GET /diagnostics`, `POST /self_test` (see below)
- `POST /invoke`
- `led.set(on)`
- `servo.pan(angle)`

## Build

Requires ESP-IDF.

```bash
idf.py set-target esp32s3
idf.py menuconfig        # dc-device robot car -> Wi-Fi SSID / password / HTTP port
idf.py build
idf.py flash monitor
```

## Wi-Fi

The app connects with `connect_wifi()` using the SSID and password from
`menuconfig`. The HTTP server starts only after an IP address is obtained.
If the SSID is empty or the connection fails, the error is logged and the
server does not start.

## HTTP endpoint

`main.cpp` registers capabilities on a `dc_device::esp32::Endpoint` and serves
it with `HttpServer` (default port 8787). Responses follow
`docs/http-transport-v0.1.md` and the Python reference server:

| Case | Status | Body |
|---|---|---|
| success | 200 | `{"ok":true,"capability":...,"result":...}` |
| malformed request | 400 | `{"ok":false,"error":"InvalidRequest",...}` |
| bad arguments | 400 | `{"ok":false,"error":"InvalidArguments",...}` |
| unknown capability | 404 | `{"ok":false,"error":"UnknownCapability",...}` |
| hardware failure | 500 | `{"ok":false,"error":"DeviceError",...}` |
| body over 2 KiB | 413 | `{"ok":false,"error":"PayloadTooLarge",...}` |

## Health

Follows `docs/health-contract-v0.1.md`. `/health` is liveness only.

- `GET /status` runs the read-only checks `led` and `servo` (did the GPIO / PWM initialise at boot).
- `POST /self_test` also runs `led_blink` and `servo_sweep`. **The servo moves** (-30, 30, then back to centre) and the LED blinks. The request takes about 1.5 s.
- `GET /diagnostics` reports uptime, free heap, and the last self-test result. It never moves anything.

```bash
curl http://<esp32-ip>:8787/status
curl -X POST http://<esp32-ip>:8787/self_test
curl http://<esp32-ip>:8787/diagnostics
```

## Invoke

```bash
curl http://<esp32-ip>:8787/manifest
curl -X POST http://<esp32-ip>:8787/invoke \
  -d '{"capability":"servo.pan","arguments":{"angle":30}}'
```

## DCO-Edge

Once the board is reachable on the LAN, point the Python reference client at:

```text
http://<esp32-ip>:8787
```

and DCO-Edge can discover the manifest and invoke the same semantic capabilities used by the Linux reference device.
