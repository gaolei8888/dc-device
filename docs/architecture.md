# dc-device Architecture

## Goal

dc-device turns physical hardware into semantic capabilities that AI agents can discover and invoke safely.

The core principle is:

> Hardware is exposed as capabilities, not vendor APIs.

## Layering

```text
LLM / Agent
   |
Tool Adapter
(MCP / native tool calling / REST)
   |
Agent Runtime
(DCO or another runtime)
   |
DCO-Edge
   |
dc-device
   |
Protocol Adapter
   |
ESP32 / Raspberry Pi / Jetson / PLC / Camera / Robot / Sensor
```

## Responsibilities

### dc-device
dc-device defines the device-facing contract:

- device identity
- capability declaration
- action invocation
- sensor reads
- events / streams
- safety metadata
- transport-neutral messages

dc-device does not depend on any LLM vendor.

### DCO-Edge
DCO-Edge is the first reference edge runtime.

It is responsible for:

- discovering dc-device endpoints
- caching capability manifests
- enforcing local permissions and safety
- low-latency execution
- offline behavior
- local small-model / rule execution
- translating capabilities into agent tools

### Agent Runtime
DCO is a first-class runtime, but not mandatory.

Other runtimes can consume the same device capabilities through adapters.

## Capability-first API

Prefer semantic operations:

```text
camera.capture()
motor.move(speed=0.3, direction="forward")
pump.read_pressure()
pump.stop()
```

Do not expose raw protocol details to the agent:

```text
write_register(0x23, 1)
```

Protocol details belong inside adapters.

## Existing protocols

dc-device sits above existing protocols instead of replacing them:

- HTTP / WebSocket
- MQTT
- Modbus
- CAN
- ROS 2
- serial
- GPIO
- vendor SDKs

## Safety

Safety is part of every capability.

A capability may define:

- parameter bounds
- confirmation requirements
- local-only execution
- emergency semantics
- rate limits
- role requirements

DCO-Edge or another runtime enforces those constraints before physical execution.

## Model independence

A dc-device capability can be translated into:

- MCP tools
- OpenAI tool schemas
- Claude tools
- Gemini function declarations
- Qwen/local-model tool calls
- custom runtimes

This translation belongs above dc-device.
