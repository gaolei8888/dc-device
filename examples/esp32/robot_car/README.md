# ESP32-S3 Robot Car Reference Device

This is the first real-hardware reference implementation for dc-device.

## Current capabilities

- `GET /health`
- `GET /manifest`
- `POST /invoke`
- `led.set(on)`
- `servo.pan(angle)`

## Build

Requires ESP-IDF.

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Wi-Fi

Wi-Fi connection setup is intentionally left application-specific in this first commit.

The device must obtain an IP address before `start_http_server()` is useful.

The next step is to add a reusable Wi-Fi transport/config module.

## DCO-Edge

Once the board is reachable on the LAN, point the Python reference client at:

```text
http://<esp32-ip>:8787
```

and DCO-Edge can discover the manifest and invoke the same semantic capabilities used by the Linux reference device.
