# Device Identity and Profile v0.1

dc-edge's onboarding flow is:

```text
discover -> identify -> load adapter/profile -> self_test -> report health -> expose capabilities
```

`identify` has to be cheap and has to work before the (larger) manifest is
fetched. That is `GET /identity`. The profile is what dc-edge then loads to
know what a device of this class should look like.

## Identity: `GET /identity`

```json
{
  "spec_version": "0.1",
  "device_id": "robot_car_01",
  "type": "mobile_robot",
  "name": "ESP32-S3 Robot Car",
  "manufacturer": "Acme",
  "model": "ESP32-S3 Robot Car",
  "hardware_revision": "B",
  "firmware_version": "0.1.0",
  "serial": "AABBCCDDEEFF",
  "profile": {"id": "esp32-s3-robot-car", "version": "0.1.0"}
}
```

- Required: `spec_version`, `device_id`, `type`, `name`. Everything else is
  optional and omitted when unknown (never an empty string).
- `device_id`, `type`, `name` **must equal** `device.id`, `device.type`,
  `device.name` in `/manifest`. `/manifest` itself is unchanged; none of the
  other identity fields appear there.
- `serial` is a stable hardware-unique id (the ESP32 uses its MAC). It is
  what lets dc-edge tell two cars with the same `device_id` apart.
- `profile` is the device-class profile the device claims to implement.
- Identity does not depend on health: it answers even when `/status` is
  `failed`.

**Identity is a claim, not proof.** There is no signature or authentication
in v0.1; anyone on the network can read it, and the MAC in `serial` is
visible to them. Authentication is the separate "device identity /
authentication" item in v0.3. Like the rest of HTTP transport v0.1, this is
for local development.

Schema: `schemas/device-identity.schema.json`.

## Profile: a device class, not a board

A profile file lives in `profiles/<id>.profile.json` and says what **every
device of one class must provide**:

```json
{
  "spec_version": "0.1",
  "profile": {"id": "esp32-s3-robot-car", "version": "0.1.0",
              "name": "ESP32-S3 Robot Car", "device_type": "mobile_robot"},
  "required_capabilities": [{"name": "led.set", "kind": "action"}],
  "optional_capabilities": [{"name": "camera.capture", "kind": "sensor"}],
  "required_health_checks": ["led", "servo"]
}
```

Profiles are published with the repository; a device does not serve its
profile, it only names it in `/identity`. dc-edge resolves `profile.id` +
`version` against its own copy of `profiles/`.

A profile contains **no pin numbers or board details**. Those stay in
`platforms/esp32/board_profiles/`, which is an implementation detail of one
firmware. (dc-device-sim's `profiles/sim.yaml` / `real.yaml` are a third
thing: deployment endpoints.)

Schema: `schemas/device-profile.schema.json`.

### Versioning

`version` is `major.minor.patch`. Adding an optional capability is a patch or
minor change; adding a required capability or required health check, or
changing a kind, is a breaking change and must bump the major version. A
device names the exact version it was built against.

### Conformance

"Conforms" means the device is **complete**, not **healthy**:

- every required capability is in the manifest with the same `kind`
- every `required_health_checks` name appears in `/status.checks`
- `profile.device_type` equals the manifest's `device.type`

A required check that is present but `failed` still conforms; `/status`
reports the failure. Missing optional capabilities are listed but never fail
conformance. The Python SDK implements this as
`dc_device.conformance(profile, manifest, status)`; dc-edge combines the
result with health and policy to decide READY / DEGRADED / BLOCKED.

## Simulator compatibility

dc-device-sim should serve `/identity` with the same `profile` reference as
the real device it stands in for, and must pass `conformance()` against that
profile. Two devices that share a profile are interchangeable to dc-edge.

## Shipped profiles

| Profile | Used by |
|---|---|
| `esp32-s3-robot-car` | `examples/esp32/robot_car` |
| `linux-reference` | `examples/linux/simulated_device.py` |

`tests/python/test_identity_profile.py` checks the Linux reference device
against its profile. The robot car firmware was checked the same way by
building its source for a host.
