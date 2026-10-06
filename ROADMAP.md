# Roadmap

## v0.1 — prove the abstraction

Goal: one real device, one edge runtime, one AI-driven physical action.

- [x] define project positioning
- [x] define architecture
- [x] define Device Capability Manifest schema
- [x] bootstrap Python SDK
- [x] bootstrap C++ SDK
- [x] define DCO-Edge integration boundary
- [x] implement HTTP transport (minimal v0.1)
- [x] implement Linux reference device
- [x] implement DCO-Edge discovery / invoke example
- [x] bootstrap ESP32 platform API
- [x] add ESP32-S3 generic board profile
- [x] add ESP32 robot-car capability manifest
- [x] add capability invocation tests
- [x] implement ESP32 HTTP endpoint
- [x] define health contract (`/status`, `/diagnostics`, `POST /self_test`)
- [x] health contract in Python SDK
- [x] health contract on ESP32
- [ ] device identity / profile
- [ ] device discovery primitives
- [ ] implement GPIO / PWM / servo backend with ESP-IDF
- [ ] implement camera capability
- [ ] add parameter validation
- [ ] add safety policy enforcement
- [ ] add WebSocket events / streams
- [ ] demo: red object -> servo + LED on real ESP32-S3

## v0.2 — useful adapters

- [ ] MQTT transport
- [ ] serial adapter
- [ ] Modbus adapter
- [ ] camera adapter
- [ ] MCP tool export adapter
- [ ] device discovery

## v0.3 — ecosystem

- [ ] TypeScript SDK
- [ ] ROS 2 adapter
- [ ] CAN adapter
- [ ] device identity / authentication
- [ ] telemetry
- [ ] remote lifecycle management
- [ ] compatibility test suite
