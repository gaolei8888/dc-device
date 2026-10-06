# ESP32 Platform

The ESP32 layer maps semantic dc-device capabilities onto ESP-IDF / Arduino hardware primitives.

The agent should see:

```text
led.set(on=true)
servo.pan(angle=30)
camera.capture()
```

It should not see:

```text
gpio_set_level(2, 1)
ledc_set_duty(...)
```

## v0.1 scope

- GPIO output
- PWM
- servo helper
- Wi-Fi transport
- HTTP capability endpoint
- board profiles

## Modules

| File | Role | Needs ESP-IDF |
|---|---|---|
| `dc_device_esp32` | GPIO / PWM / servo | on device only |
| `dc_device_wifi` | Wi-Fi station | on device only |
| `dc_device_json` | exception-free JSON parser/writer | no |
| `dc_device_endpoint` | HTTP Transport v0.1 + health contract core: `(method, path, body) -> response` | no |
| `dc_device_http_server` | binds an `Endpoint` to `esp_http_server` | on device only |

The endpoint core has host tests: `tests/esp32/run.sh`.

## Principle

Pin numbers are device implementation details. They belong in board/device profiles, not in the AI-facing manifest.
