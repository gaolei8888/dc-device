# Hardware Discovery & AI-Assisted Device Adaptation (Proposal)

Status: **Design proposal — not implemented**  
Date: 2026-10-15  
Scope: `dc-device` with integrations to `dc-device-sim` and `dc-edge`

## 1. Motivation and references

The existing dc-device mission is to make real hardware discoverable, health-aware, and usable through safe semantic capabilities by model-agnostic agents. Current onboarding assumes a recognized device profile or an adapter. The next step is **assisted onboarding for unfamiliar hardware**, without presenting inferred protocols as verified facts.

Christian F. K. Schaller described using an AI-assisted reference-and-diff workflow to build a Linux SANE scanner backend and improve a laptop MIPI camera stack. His later, generalized [hw-harness](https://github.com/cschalle/hw-harness) captures known-good USB traffic with a vendor driver in a Windows guest (using host usbmon), compares it with Linux implementation traces, and supports non-USB evidence such as INF/ACPI/device properties. The author explicitly notes that this public generalized harness was developed *after* the original experiments, and that physical validation remained essential.

- [Original GNOME article (2026-10-15)](https://blogs.gnome.org/uraeus/2026/10/15/aiandreverseengineerhardware/)
- [hw-harness source](https://github.com/cschalle/hw-harness)

**Design lesson:** reference traces + repeatable experiments + observable differences are far more useful than asking an LLM to guess an undocumented command sequence. This is a proposal inspired by that approach, not a claim of working plug-and-play reverse engineering.

## 2. Responsibilities and boundaries

- **dc-device (this repo):** discovery primitives, identity fingerprints, evidence format, protocol/adapter candidate representation, capability manifest, `self_test()`/`health_check()`/`get_status()`/`diagnostics()` contracts, and a process for promoting *validated* adapters. It does not autonomously approve dangerous actions.
- **dc-device-sim:** protocol trace replay, virtual-device behavior models, fault injection, differential tests, and recording coverage/unknown behavior. A simulator built from observed traces is **not** evidence that unobserved device behavior is safe.
- **dc-edge:** runtime device discovery/orchestration, physical safety enforcement, authorization, sandbox/test lifecycle, READY/DEGRADED/BLOCKED assessments and verified deployment. It is the policy gate for any real action.
- **DCT / other developer agents:** optional analysis, hypothesis generation, adapter code generation, tests and review. Model/provider independent.
- **DCO / user-facing agents:** optional plain-language workflow and explanations; cannot bypass dc-edge permissions.

Keep existing `dc-device` capability-first contracts and offline-compatible adapters intact. Treat model inference as an optional development aid, not as a runtime dependency.

## 3. Proposed discovery pipeline

```text
connect device
  -> enumerate & fingerprint (passive/read-only)
  -> match trusted profile/adapter
      -> matched: standardized self-test -> capability registration
      -> unknown:
          collect authorized evidence and reference operations
          -> normalize traces/metadata
          -> compare baseline vs candidate behavior
          -> AI-assisted hypotheses (clearly marked unverified)
          -> generate adapter + tests
          -> replay / fault-injection in dc-device-sim
          -> guarded real-device verification through dc-edge
          -> human review / signed versioned adapter
          -> capability registration + health checks
```

Discovery should still work with no AI model connected: the result for an unknown device can be `UNSUPPORTED` plus a reproducible evidence bundle.

## 4. Proposed contracts (illustrative; not committed schemas)

### Device fingerprint

```yaml
device_id: scanner_001
transport: usb
identity:
  vendor_id: "07b3"
  product_id: "1825"
  revision: unknown
  serial_hash: "<redacted-hash>"
capability_status: unknown
adapter:
  match_state: unmatched
  trust: none
evidence:
  consent_id: session-001
  bundle_id: capture-001
```

Future transport-specific fingerprint fields: USB descriptors and interfaces; HID report descriptors; serial VID/PID, port attributes, and safe negotiated parameters; network discovery; BLE advertisements. Do not probe active functions or change device state during passive identification.

### Evidence bundle

Standardize a portable, versioned record containing: transport, OS/runtime, timestamps, reference/candidate capture IDs, chosen operation, input parameters, observed messages, parser version, redaction policy, and provenance. Annotate directions, transfer types, packet ordering, timing, checksums and confidence where available. Never require cloud upload for local analysis.

### Adapter candidate state

`DISCOVERED -> EVIDENCE_COLLECTED -> HYPOTHESIZED -> SIM_VALIDATED -> HARDWARE_VALIDATED -> APPROVED`

`REJECTED` / `QUARANTINED` are possible from any unapproved stage. Generated code and inferred commands cannot be executed as trusted production capabilities before verification and approval.

### Capability readiness

Expose an explicit distinction between **device health**, **adapter confidence**, and **system authorization**. A successful probe does not imply a safe `motor.move` capability. Store provenance and validation coverage per capability.

## 5. Safety and governance requirements

1. **Explicit authorization** for captures, VM/device passthrough, probes, firmware access and generated-driver tests. Observe vendor licenses and applicable law; do not copy proprietary driver source.
2. **Read-only first**. Maintain a transport-specific allowlist for any write/control operations. Unknown opcodes are denied by default.
3. **Physical safeguards** for actuators: power/current/speed bounds, watchdog, dead-man/stop controls, limits, isolation and supervised test runs. Never rely on an LLM as an emergency-stop mechanism.
4. **Least privilege**: generated code executes in a sandbox with no arbitrary system/device access; versioned signed/approved adapters only in production.
5. **Privacy:** redact serial numbers, identifiers, user data, captured content and secrets; encrypt bundles where stored, and require consent before sharing externally.
6. **Reproducibility:** record test inputs, environment versions, firmware revision and device identity; reject regressions and report uncertainty/coverage.
7. **Failure state:** fail closed. dc-edge retains final READY/DEGRADED/BLOCKED decision; dc-device only reports health and capability evidence.

## 6. Implementation roadmap

### P0 — USB/HID evidence-oriented proof of concept

- Passive USB/HID enumeration, identity fingerprint and known-adapter matching.
- Local evidence bundle format and trace import (initially offline files, not arbitrary live probing).
- Trace normalization and diff CLI with deterministic, human-readable comparison.
- Unknown-device status and standard health/diagnostics messages.
- Sample **non-actuating** target (HID input device or read-only USB sensor) and test fixture.
- Explicit test: same operation/reference traces produce explainable differences, with no unsupported capability registered automatically.

**Acceptance:** onboard a known device without AI; identify an unknown one without writes; record and compare traces reproducibly; generate diagnostics and never silently enable unverified commands.

### P1 — Assisted adapter generation

- Optional local/remote LLM plugin to propose protocol hypotheses, adapter skeletons and unit tests from evidence.
- Static checks and repeatable simulator replay via dc-device-sim.
- Human-review workflow with per-command evidence and confidence labels.
- One approved adapter demonstrates capability registration, self-test and observable error handling.

**Acceptance:** proposals are reviewable and reversible; no generated adapter is promoted merely because a model claims success.

### P2 — Broader transport and one-click integration

- Serial, BLE and network adapters; later Modbus/CAN/ROS 2 where appropriate.
- dc-edge-managed supervised physical verification and policy-aware deployment.
- One-click supported-device readiness; guided evidence collection for unknown devices.
- Shared conformance suites across real and simulated devices.

**Acceptance:** dc-edge reports READY/DEGRADED/BLOCKED on evidence; simulations flag unsupported or uncovered behaviors rather than inventing responses.

## 7. Suggested code organization (future, not created by this document)

```text
dc-device/
  discovery/           # passive enumeration, fingerprints, trusted matching
  evidence/            # capture imports, redaction, provenance, trace diff
  adapter_lab/         # hypotheses, generated candidates, verification metadata
  schemas/             # identity/evidence/adapter-state contracts
  tests/fixtures/      # approved, sanitized traces
  docs/hardware-discovery-ai-assisted-adaptation.md
```

Start as libraries/interfaces within the current SDK before introducing a new standalone service. Do not duplicate dc-edge's orchestration or dc-device-sim's simulation responsibilities.

## 8. Immediate engineering tasks

- [ ] Audit existing discovery/identity/health schema and name compatible extension points.
- [ ] Define `device_fingerprint`, `evidence_bundle`, `adapter_candidate` with redaction and state semantics.
- [ ] Prototype offline USB trace import + normalization + structured diff.
- [ ] Add sanitized reference/candidate traces and deterministic tests.
- [ ] Define simulator replay interchange with dc-device-sim.
- [ ] Define dc-edge approval gate for any hardware write and adapter promotion.
- [ ] Run one supervised, non-actuating proof of concept; publish limitations and coverage.

## 9. Non-goals

- Universal driver generation on first plug-in.
- Autonomous firmware flashing or unbounded active probing.
- Production-ready claims based on synthetic data alone.
- Replacing device vendor SDKs or existing protocol stacks.
- Coupling dc-device to a specific model, UI, or cloud service.
