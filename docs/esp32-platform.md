# ESP32 Platform Model

dc-device does not replace ESP-IDF or Arduino.

It adds a semantic AI-facing capability layer above them.

```text
Agent / DCO
    |
DCO-Edge
    |
dc-device capability
    |
ESP32 adapter
    |
ESP-IDF / Arduino
    |
GPIO / PWM / I2C / SPI / UART / Camera
```

## v0.1 hardware abstractions

- GPIO output
- PWM output
- hobby servo

## Next hardware abstractions

- GPIO input
- ADC
- I2C sensor
- UART
- camera
- motor driver

## Pin policy

Pin numbers are implementation details.

They belong in board or device profiles, not in the agent-facing capability schema.

A capability should remain stable when the physical pin mapping changes.
