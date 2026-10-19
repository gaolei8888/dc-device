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

## Moving devices must have a safe state (gap 11)

A capability that can cause physical motion, or other hazardous actuation,
declares `safety.motion: true`. The rule:

> A device with any `motion` capability must declare `safety_kernel.safe_state`,
> and that capability must be an `emergency` capability that needs no arguments.

It is checked in three places:

- `validate_safety()` fails (Python `serve()` refuses to start; ESP32 firmware
  must not serve).
- `conformance()` reports `missing_safe_state: true` and `conforms: false`, so a
  consumer does not judge the device ready. It also fails when `safe_state`
  names a capability that is not `emergency` in the manifest.
- Devices without any `motion` capability need none of this.

`motion` is a declaration by the device author. The library cannot tell whether
an LED or a motor is behind a handler, so a device that forgets to mark a
moving capability escapes the rule. Profiles for moving device classes should
list the stop capability as required, so omission shows up as a missing
capability as well. (The shipped robot car profile does not yet.)

## Running the watchdog

The kernel only acts when `tick()` is called.

- Python: `serve()` starts a thread that calls `Device.tick()` every 50 ms.
  Code that builds its own server must do the same.
- ESP32: call `Endpoint::tick()` from a task about every 50 ms. `Endpoint` is
  not thread-safe, so guard `handle()` and `tick()` with one mutex. The
  worst-case delay before the safe state runs is the longer of the tick period
  and any handler that is currently running.

**Status on ESP32 (2026-10-18):** `HttpServer` owns one mutex around `handle()` and runs a
`dc_tick` task that calls `Endpoint::tick()` every `tick_period_ms` (default 50). Before
this change nothing called `tick()`.

Measured on the third board with the `debug.hold` test fixture in
`examples/esp32/camera_board` (`max_duration_ms` 2000, safe state `debug.release`, times
taken on the device's own clock):

| Case | Result |
|---|---|
| Hold, send nothing | Safe state ran after 2014 to 2042 ms (6 runs): 14 to 42 ms late |
| Hold, then release by hand at 0.5 s | Released once; the watchdog did not fire again |
| Hold, then a full self test starts | Safe state ran after **8390 ms**, about 6.4 s late |

The last row was measured before the fix below. A request holds the mutex while it runs,
so the watchdog waits for it; a full self test takes about 8 s and stretched the time
limit by that much.

**Fix: `POST /self_test` answers 409 `Busy` while a time-limited capability is running**
(`Endpoint::time_limited_running()`, Python `Device.time_limited_running()`). On the
third board: hold, then self test, gave 409 after 32 ms; `/status` stayed 200; after the
hold timed out a self test ran normally. Other long requests are not covered: a capability
handler that blocks for seconds still delays the watchdog and any stop request, so
handlers of moving capabilities must return quickly.

Timing is only as good as the tick period. A capability whose
`max_duration_ms` is shorter than about twice the tick period is not
meaningful.

## Not covered here

- **Stale-command rejection**: gap 12 (`issued_at` / `valid_ms`). The two work
  together: freshness rejects an old command arriving late, the kernel stops an
  old command that is still running.
- **Hardware watchdogs and brown-out behavior**: firmware concerns, not
  something the contract can express yet.
- The reference ESP32 robot car marks `servo.pan` as `motion`, declares
  `servo.stop` (stops the PWM pulse train, so the servo goes limp) as its safe
  state, and has no `max_duration_ms`. It has not been run on hardware.
