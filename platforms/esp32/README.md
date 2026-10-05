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

## Principle

Pin numbers are device implementation details. They belong in board/device profiles, not in the AI-facing manifest.
