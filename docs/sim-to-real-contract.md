# Sim-to-Real Contract

dc-device and dc-device-sim share one rule:

> A capability that exists in simulation must keep the same semantic name and input/output schema on real hardware.

Example:

```text
servo.pan(angle=30)
```

Simulation implementation:
- update simulated joint state

ESP32 implementation:
- convert angle to PWM pulse

Agent-facing contract:
- unchanged

This lets DCO-Edge validate a workflow in dc-device-sim and then deploy the same workflow to real hardware by changing only the device endpoint / adapter.

## Compatibility requirements

A simulated and real device are considered compatible when they share:

- device type
- capability names
- parameter schemas
- result schemas
- declared safety constraints

Hardware-specific metadata and pin mappings do not need to match.
