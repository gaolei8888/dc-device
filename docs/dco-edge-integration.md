# DCO-Edge Integration

DCO-Edge is the first reference runtime for dc-device.

## Contract

DCO-Edge should be able to:

1. discover a dc-device endpoint
2. fetch its Device Capability Manifest
3. register capabilities as local tools
4. apply permissions and safety policy
5. invoke a capability
6. receive structured results and events
7. expose selected capabilities to DCO or another agent runtime

## Separation of concerns

dc-device:
- describes and invokes hardware capabilities
- remains model-agnostic

DCO-Edge:
- executes locally
- enforces safety
- handles offline behavior
- may run small models / rules
- adapts capabilities to agent tool formats

DCO:
- performs higher-level planning and orchestration
- is optional from dc-device's perspective

## First integration demo

Target:

> An ESP32-S3 exposes camera, LED, and servo capabilities. DCO-Edge discovers them and executes: "If a red object is visible, point the servo toward it and turn on the LED."

This proves:

```text
AI intent
  -> agent plan
  -> DCO-Edge
  -> dc-device capability
  -> physical action
```
