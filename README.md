# dc-device

**Connect any device to any AI agent.**

`dc-device` is an open device capability layer for turning real-world hardware into safe, discoverable tools that AI agents can use.

The project is intentionally **model-agnostic** and **agent-agnostic**. It does not bind devices to OpenAI, Claude, Gemini, Qwen, DCO, or any single runtime. Instead, it defines a common device capability model that runtimes such as **DCO-Edge** can expose to any agent or tool-calling system.

## Why

Today, AI agents can operate software, browsers, and APIs, but physical devices remain fragmented across GPIO, serial, MQTT, Modbus, CAN, ROS 2, HTTP, vendor SDKs, and custom protocols.

`dc-device` provides one common layer:

```text
AI model / Agent
      |
Agent runtime / Adapter
      |
   DCO-Edge
      |
  dc-device
      |
ESP32 / Raspberry Pi / Jetson / PLC / Camera / Robot / Sensor
```

The goal is simple:

> **Make a physical device as easy for an AI agent to use as a software tool.**

## Design principles

1. **Model-agnostic** — no dependency on a specific LLM provider.
2. **Agent-agnostic** — DCO is a first-class runtime, not a mandatory one.
3. **Hardware-agnostic** — ESP32, Linux, PLCs, robots, cameras, sensors, and more.
4. **Capability-first** — agents see semantic actions such as `move()`, `capture()`, or `read_pressure()`, not raw registers.
5. **Safety-first** — permissions, limits, confirmation rules, and emergency actions are part of the capability description.
6. **Offline-friendly** — local execution remains possible without a cloud model.
7. **Reuse existing protocols** — dc-device sits above MQTT, Modbus, CAN, ROS 2, HTTP, USB, serial, and vendor protocols instead of replacing them.
8. **Health-aware** — every device should expose a standard self-test, health status, and diagnostics contract.

## Core concept: Device Capability Manifest

A device describes what it can sense and do:

```yaml
device:
  id: robot_car_01
  type: mobile_robot
  name: Classroom Robot Car

capabilities:
  - name: camera.capture
    description: Capture one frame from the front camera
    kind: sensor

  - name: motor.move
    description: Move the robot
    kind: action
    parameters:
      speed:
        type: number
        minimum: 0
        maximum: 1
      direction:
        type: string
        enum: [forward, backward]

  - name: motor.stop
    description: Stop the robot immediately
    kind: action
    safety:
      local_allowed: true
      emergency: true
```

An agent runtime can translate the same manifest into MCP tools, OpenAI tools, Claude tools, Gemini function calls, or a local model's tool schema.

## Standard device health contract

Hardware-specific self-test execution belongs in `dc-device`. A device adapter should expose a common health interface such as:

```text
self_test()
health_check()
get_status()
diagnostics()
```

A normalized result should distinguish device health from system-level task decisions:

```yaml
device_id: camera_01
status: degraded

checks:
  connection: ok
  stream: ok
  fps: warning
  temperature: ok

issues:
  - code: LOW_FPS
    severity: warning
    message: fps below expected threshold
```

`dc-device` answers: **"Is this device healthy, and what exactly is wrong?"**

It does not decide whether the whole system may proceed. That decision belongs to `dc-edge`.

## One-click hardware readiness goal

The three hardware projects share a product-level goal:

> **A user should be able to connect multiple devices and complete discovery, identification, self-test, health reporting, capability registration, and readiness assessment with one action.**

For `dc-device`, this means every supported device must provide enough standardized information for `dc-edge` to automate:

```text
discover
→ identify
→ load adapter/profile
→ self_test
→ report health
→ expose capabilities
```

The long-term target is zero-touch device onboarding: users should not need to understand ports, drivers, IP addresses, vendor SDK details, or protocol internals for normal supported hardware.

## Initial scope

### v0.1
- Device Capability Manifest
- standardized device identity / profile
- standardized self-test and health contract
- Python SDK
- C++ SDK
- Linux reference device
- ESP32 reference device
- DCO-Edge reference adapter
- HTTP / WebSocket transport
- basic safety constraints
- device discovery primitives
- device simulator compatibility

### Later
- MQTT transport
- Modbus adapter
- CAN adapter
- ROS 2 adapter
- automatic adapter matching
- authentication / identity
- telemetry and events
- remote lifecycle management
- TypeScript SDK

## Repository layout

```text
dc-device/
├── schemas/          # capability, device, health and diagnostics schemas
├── sdk/
│   ├── python/       # Python SDK
│   └── cpp/          # C++ SDK
├── adapters/         # protocol and runtime adapters
├── examples/
│   ├── linux/
│   └── esp32/
├── docs/
└── tests/
```

## Relationship with dc-edge and dc-device-sim

- **dc-device** defines how real devices describe capabilities, identity, health, self-test and diagnostics.
- **dc-edge** discovers devices, launches one-click system checks, applies policy and safety, orchestrates devices, and decides READY / DEGRADED / BLOCKED.
- **dc-device-sim** implements the same contracts for virtual devices and adds controlled fault injection so system recovery can be tested before touching real hardware.

```text
                  dc-edge
                     |
             capability + health
                     |
          +----------+----------+
          |                     |
      dc-device          dc-device-sim
          |                     |
   real hardware         virtual hardware
```

## What dc-device is not

- not another Modbus
- not another MQTT
- not a replacement for ROS 2
- not an LLM SDK
- not tied to DCT
- not tied to a cloud model

It is the **AI-facing physical capability and device-health abstraction layer** above those technologies.

## First demo target

The first end-to-end demo should be deliberately small:

> An ESP32-S3 device registers a camera/LED/servo capability, DCO-Edge discovers it, runs its self-test automatically, and an agent can say: **"If you see a red object, point the servo toward it and turn on the LED."**

That demo proves the full path from **device discovery → self-test → readiness → AI intent → safe physical action**.

## Status

Early design / bootstrap stage.

Contributions and experiments are welcome.
