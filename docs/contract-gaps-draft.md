# Contract Gaps (DRAFT)

Status: draft for discussion, 2026-10-11. Not a decision. Nothing here is implemented.

Sources: this repo's `ROADMAP.md` and v0.1 docs, `dc-octo/docs/2026-10-03-dco-edge-physical-capability-runtime.md`,
and Meta's open-source Muse Gadgets SDK (`facebookincubator/muse-gadget-sdk`, `esp32/AGENTS.md`,
`linux/AGENTS.md`; read as a summary, not from source).

Each item says what is missing, why, and whether `ROADMAP.md` already lists it.
"Roadmap" column: yes / partial / no.

## A. Contract semantics (transport-independent)

| # | Gap | Why | Roadmap |
|---|---|---|---|
| 1 | Async commands: `/invoke` may answer `{"ok":true,"pending":true,"job_id":...}`; result fetched or pushed later | Long actions must not block the HTTP task. ESP32 `self_test` runs synchronously (~1.5 s) in the HTTP task | no |
| 2 | Receipt on success: `receipt` (what actually ran, reached target or not, stop reason) beside `result` | `command -> receipt -> verification` (DCO Edge doc §6). The caller verifies; it should not trust a bare `ok` | no |
| 3 | Reading age: sensor readings as `{value, unit, age_s}`, `null` when stale | Stale data used as fresh is a physical-world bug. Muse `sensors.read` does this | no |
| 4 | Session generation on requests/results; stale results dropped | Results from an earlier session must not leak into a new one | no |
| 5 | Management commands (reboot, OTA, unpair) kept out of the manifest | Agent must not see them. Muse enforces this with a test | partial (v0.3 remote lifecycle) |
| 6 | Manifest size cap (Muse: 8 KB for register) | MCU memory | no |
| 7 | One error-body shape across Python and ESP32 | Today clients must tolerate two shapes | no |
| 8 | Parameter validation against the manifest schema, on device or specified as required handler behavior | Muse firmware also does not enforce the schema; handlers check themselves | yes (v0.1, open) |

## B. Safety

| # | Gap | Why | Roadmap |
|---|---|---|---|
| 9 | Make `safety` enforceable and map it to dct/DCO tiers (including `Physical`): define exact meaning of `requires_confirmation`, `local_allowed`, `emergency`, `max_rate_hz` | Declared today, enforced nowhere | yes (v0.1, open) |
| 10 | Local safety kernel declaration: `max_duration_ms`, watchdog, disconnect behavior, stale-command rejection, safe boot state | Must live on the device; an edge layer cannot stop a motor in time over Wi-Fi + LLM | no |
| 11 | Mandatory `stop` capability (`emergency: true`) on any device that can move | A device with no way to stop should not be judged ready | no |
| 12 | Command freshness: `/invoke` carries `issued_at` and `valid_ms`; device rejects expired commands | A delayed command must not suddenly execute | no |
| 13 | Ticket pass-through: optional `ticket` (or digest) on `/invoke`, validated or at least recorded | Align physical actions with dco execution tickets | no |
| 14 | Logging rule: record that a command ran and how it ended, never parameters or output | Muse Linux does this | no |

## C. Identity, connection, discovery

| # | Gap | Why | Roadmap |
|---|---|---|---|
| 15 | Authentication; `/identity` is unauthenticated and exposes MAC as `serial` | v0.1 is local-dev only | yes (v0.3) |
| 16 | Pairing with physical confirmation; afterwards only the paired key is trusted | Muse community pairing v5 needs a button press | no |
| 17 | Discovery primitives (mDNS or similar) | Endpoints are configured by hand today | yes (open) |
| 18 | Optional device-initiated outbound session | Muse: device dials the agent over Noise, which works behind NAT. DCO docs say "dct connects out". **Direction is undecided** | no |
| 19 | Events / streams over WebSocket | Otherwise only polling | yes |

## D. Health and profiles

| # | Gap | Why |
|---|---|---|
| 20 | `checked_at` / age on each health check | Needed to know whether a health result can be trusted |
| 21 | How a profile `id@version` is distributed to the consumer | Open question from the handover |
| 22 | Policy when a declared profile cannot be found locally (DEGRADED vs BLOCKED) | Same |
| 23 | One vocabulary for the system verdict (BLOCKED here, FAILED in dc-device-sim) | Two repos disagree |

## Suggested order

1. Safety and correctness first: 1, 2, 3, 8, 9, 10, 11, 12.
2. Needed to leave local dev: 7, 15, 16, 17, 19.
3. Spec hygiene, any time: 4, 5, 6, 13, 14, 20-23.

## Not verified

- Items 9 and 12 need field-level design against `schemas/device-capability.schema.json`
  (current `safety` properties: `requires_confirmation`, `local_allowed`, `emergency`, `max_rate_hz`).
- Muse details come from fetched summaries of its `AGENTS.md` files, not from reading its source.
- Item 18 is a design decision, not a technical one.
