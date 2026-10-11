# Safety semantics v0.1

Status: draft. Resolves the part of gap 9 that a device can decide for itself.
The tier mapping at the end is a proposal for the dct/DCO owners, not a
decision.

## Principle

Each `safety` field is either **enforced by the device** or **an obligation on
the caller**. A field must never be described as enforced when the device has
no way to check it. A device that can only declare a rule says so.

| Field | Who enforces | Meaning |
|---|---|---|
| `max_rate_hz` | Device | Minimum interval between accepted invocations of this capability is `1 / max_rate_hz` seconds. |
| `emergency` | Device (registration) and caller | The capability stops or makes the device safe. It is never rate limited, and it cannot require confirmation. |
| `requires_confirmation` | Caller | The caller must have an explicit, current confirmation from the user for this invocation before sending it. The device does not verify it (no proof is carried on `/invoke`; see gaps 12 and 13). |
| `local_allowed` | Caller | See below. Declared only. |

Unknown fields under `safety` are ignored (the schema allows them).

## `max_rate_hz`

- Must be a number greater than 0. A device refuses to register a capability
  with any other value (a silently ignored limit is worse than none).
- Counted per capability, not per caller.
- Only invocations that reach the handler use up the budget. Requests rejected
  for malformed JSON, an unknown capability or arguments that fail the manifest
  schema do not. A handler that returns its own `InvalidArguments` does.
- A rejected call returns HTTP 429, `{"ok":false,"error":"RateLimited","message":"... retry in N ms"}`.
  The message carries the wait; there is no separate field yet (gap 7).
- A rejected call does not move the window. Retrying after the stated wait works.

## `emergency`

- Must be a boolean. A device refuses to register an emergency capability that
  has `requires_confirmation: true`: a stop that waits for a confirmation is not
  a stop.
- Exempt from `max_rate_hz` even if one is declared.
- Whether a device that can move **must** have one is gap 11, not decided here.

## `requires_confirmation`

The device cannot tell a confirmed call from an unconfirmed one. Until
`/invoke` carries a verifiable proof (gap 13), this is a promise by the caller.
Consumers that skip it are non-conforming; the device will not catch them.

## `local_allowed`

Declared only, and the current wording is ambiguous. Proposed meaning: `false`
means the owner has marked the capability as one that must go through the
approval path and must not be called directly by any caller on the local
network. The device cannot see where a request came from, so this is
enforceable only by the consumer. If the owners of dct/DCO do not need it, drop
the field instead of keeping a rule nobody checks.

## Proposed mapping to dct/DCO tiers (not decided)

The DCO ladder in `dc-octo` (read < self < publish < irreversible < money) was
written for screen actions and has no tier for physical effects.

| Device declaration | Proposed handling by the consumer |
|---|---|
| `kind: sensor` | Read tier. No confirmation. |
| `kind: action`, no `requires_confirmation` | New "physical" tier: needs a standing user grant for this device and capability; no per-call prompt. |
| `kind: action`, `requires_confirmation: true` | Per-call user confirmation, at the strength of the irreversible tier. |
| `emergency: true` | Always allowed. Never blocked by a missing grant. |

Open for the dct/DCO owners: whether "physical" is a new tier, and what a
standing grant looks like.

## Conformance

- Python SDK: `Device.register` raises `ValueError` for a bad `safety`;
  `Device.invoke` raises `RateLimited`, which the HTTP server maps to 429.
- ESP32: `Endpoint::add` returns false for a bad `safety`; `/invoke` answers 429.
- Both are covered by tests (`tests/python/test_device.py`,
  `tests/esp32/test_endpoint.cpp`).
