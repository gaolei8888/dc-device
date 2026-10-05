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

## Initial scope

### v0.1
- Device Capability Manifest
- Python SDK
- C++ SDK
- Linux reference device
- ESP32 reference device
- DCO-Edge reference adapter
- HTTP / WebSocket transport
- basic safety constraints
- device simulator

### Later
- MQTT transport
- Modbus adapter
- CAN adapter
- ROS 2 adapter
- device discovery
- authentication / identity
- telemetry and events
- remote lifecycle management
- TypeScript SDK

## Repository layout

```text
dc-device/
├── schemas/          # capability and device schemas
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

## Relationship with DCO-Edge

`DCO-Edge` is the first reference runtime for `dc-device`.

- **dc-device** defines how devices describe and expose capabilities.
- **DCO-Edge** discovers those capabilities, applies local policy and safety, executes tasks, and exposes them to agents.
- **DCO** may orchestrate higher-level tasks, but dc-device does not require DCO.

```text
DCO / Other Agent Runtime
          |
      DCO-Edge
          |
      dc-device
          |
       Devices
```

## What dc-device is not

- not another Modbus
- not another MQTT
- not a replacement for ROS 2
- not an LLM SDK
- not tied to DCT
- not tied to a cloud model

It is the **AI-facing device abstraction layer** above those technologies.

## First demo target

The first end-to-end demo should be deliberately small:

> An ESP32-S3 device registers a camera/LED/servo capability, DCO-Edge discovers it, and an agent can say: **"If you see a red object, point the servo toward it and turn on the LED."**

That demo proves the full path from **AI intent → device capability → safe physical action**.

## Status

Early design / bootstrap stage.

Contributions and experiments are welcome.
