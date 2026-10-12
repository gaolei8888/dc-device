# Safety kernel v0.1

Status: draft. Resolves gap 10 for the parts a transport-independent device
library can enforce. Read `safety-semantics-v0.1.md` first.

## Why it lives on the device

A controller that reaches the device over Wi-Fi and an LLM cannot stop a motor
in time, and cannot stop it at all once it has disconnected. So the rule that
brings the device back to a safe state must run on the device and must not
depend on a later message from the controller.

## The rule: a time-limited action falls back to the safe state

A capability may declare `safety.max_duration_ms` (integer, > 0). After a
successful invocation, the device holds its effect for at most that long. If
the capability is not invoked again within that time, the device runs its
**safe state**.

- Re-invoking the capability restarts the clock. A controller that wants a
  motor to keep running keeps sending the command. If it crashes, loses Wi-Fi
  or is delayed, the motor stops by itself. This is the watchdog, and it is
  also the disconnect behavior: the device never needs to know the controller
  is gone.
- Invoking the safe-state capability by hand disarms every pending timer.
- The safe state runs once per expiry, then waits for a new invocation.
- A failed invocation does not start the clock.
- An `emergency` capability cannot be time limited (registration is refused).

## The safe state

The device names one capability, an `emergency` capability that needs no
arguments (for example `motor.stop`), as its safe state:

```json
"safety_kernel": { "safe_state": "motor.stop", "safe_on_boot": true }
```

`safety_kernel` is an optional top-level manifest field. `safe_state` must name
a registered emergency capability with no required arguments.

`safe_on_boot` is a declaration, not something the library can enforce: the
firmware promises that actuators are in the safe state before it starts
serving. A consumer may rely on it only as far as it trusts the firmware.

A device that declares any `max_duration_ms` must have a safe state.
`validate_safety()` checks this. The Python `serve()` calls it and refuses to
start; ESP32 firmware must call it once after registration and not serve if it
returns false.

## Running the watchdog

The kernel only acts when `tick()` is called.

- Python: `serve()` starts a thread that calls `Device.tick()` every 50 ms.
  Code that builds its own server must do the same.
- ESP32: call `Endpoint::tick()` from a task about every 50 ms. `Endpoint` is
  not thread-safe, so guard `handle()` and `tick()` with one mutex. The
  worst-case delay before the safe state runs is the longer of the tick period
  and any handler that is currently running.

Timing is only as good as the tick period. A capability whose
`max_duration_ms` is shorter than about twice the tick period is not
meaningful.

## Not covered here

- **Stale-command rejection**: gap 12 (`issued_at` / `valid_ms`). The two work
  together: freshness rejects an old command arriving late, the kernel stops an
  old command that is still running.
- **Mandatory stop capability** on any device that can move: gap 11. Today
  nothing forces a moving device to declare `safety_kernel`.
- **Hardware watchdogs and brown-out behavior**: firmware concerns, not
  something the contract can express yet.
- The reference ESP32 robot car declares no `max_duration_ms` and no stop
  capability yet.
