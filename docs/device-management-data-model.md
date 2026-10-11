# Device management: data model (draft)

Status: draft. Not part of the wire contract. This describes what any device
management UI needs to know about a device, and where each piece comes from.
It is host-agnostic: it does not assume `dc_desktop`, `dct` or any UI
framework, so it survives a rewrite of the host application.

Scope: view and self-test. Sending commands to a device and approving them
belong to dct/DCO ("dct decides. dco acts."), not to this layer.

## Principle

A device record is a **cache of what the device said about itself**, plus a few
things the user added. Anything the device can report is re-read from the
device and never edited by the user. If the two disagree, the device wins.

## Record

| Group | Field | Source | Notes |
|---|---|---|---|
| Key | `serial` | `GET /identity` | Primary key. Stable hardware id (the MAC on ESP32). |
| | `device_id` | `GET /identity` | Not unique across devices; do not use as key. If `serial` is absent, fall back to (`device_id`, address) and mark the record weakly identified. |
| Identity | `type`, `name`, `manufacturer`, `model`, `hardware_revision`, `firmware_version` | `GET /identity` | Descriptive only. Not authentication. |
| Connection | `address` (host and port) | user input today | Later: discovery (gap 17) or pairing. Not provided by the device. |
| | `last_seen_at` | host | Time of the last successful request. |
| | `reachable` | host | Derived: the last request succeeded. `/health` is the cheap probe. |
| Capabilities | list of `name`, `kind`, `description` | `GET /manifest` | Re-read when `firmware_version` changes. |
| Profile | `profile.id`, `profile.version` | `GET /identity` | Declared by the device. |
| | `conforms`, `missing_capabilities`, `kind_mismatches`, `missing_health_checks`, `type_mismatch` | host | Computed by comparing manifest and status with the profile (`conformance()` in `sdk/python/dc_device/profile.py`). Completeness, not health. |
| Health | `status` (`ok` / `degraded` / `failed`), `checks`, `issues` | `GET /status` | Cheap and read-only; safe to poll. |
| | `checked_at` | host | The device does not send this yet (gap 20). |
| Self-test | last report, `ran_at` | `POST /self_test` | May move hardware. Only on explicit user action, never on a timer. |
| Diagnostics | `uptime_s`, `capability_count`, `info` | `GET /diagnostics` | Read-only. Never runs a self-test. |
| User | `display_name` | user | Only field the user may edit that is not a connection setting. |
| | project role bindings | user | Which project uses this device for which role. Kept from the old ledger. |

## States to show

A user should see one of these per device, derived in this order:

1. **Unreachable**: the last request failed. Show `last_seen_at`.
2. **Failed**: `status = failed`. Show the issue messages.
3. **Degraded**: `status = degraded`. Show the issue messages.
4. **Not up to profile**: `status = ok` but `conforms = false`.
5. **No profile**: reachable and healthy, but the device declares none.
6. **Ready**: `status = ok` and `conforms = true`.

"No profile" is not an error. Whether an unknown profile should block use is
policy and is gap 22, not decided here.

## Polling

- Poll `/status` on a timer (suggested 10 s while the device page is open,
  60 s otherwise). It is cheap and read-only.
- Re-read `/identity` and `/manifest` on first connect and when the reported
  `firmware_version` changes.
- Run `/self_test` only when the user presses the button. It actuates
  hardware, is serialized on the device, and a second request can be refused
  or queued.
- Read `/diagnostics` only when the detail view is open.
- Treat HTTP 200 as "a report was produced", not "device healthy". Read the
  `status` field.

## Self-test results

The device returns the same report shape as `/status`, with the actuating
checks added. Store the whole report with a timestamp. Do not translate it into
a per-peripheral pass/fail table: the check names are device-defined.

## What the old ledger does not map to

The `dc_desktop` serial ledger assumes a fixed board enum, a fixed firmware
family enum, a serial-port locator and per-peripheral pass/fail results. None
of these exist for a `dc-device` device. Only the display name and the project
role bindings carry over.

## Open items

- How the address is obtained: manual entry, discovery (gap 17) or the
  device connecting out (gap 18).
- Whether the host or the device owns `checked_at` (gap 20).
- Where profiles are distributed from (gap 21), so the host can find the profile a
  device names, and what to do when it cannot (gap 22).
- Authentication: until pairing exists (gaps 15 and 16), anyone who can reach the
  address can call it. The UI must not imply the connection is secured.
