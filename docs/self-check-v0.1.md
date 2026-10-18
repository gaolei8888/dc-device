# Self-check v0.1

Status: draft. Implemented and run on one real ESP32-S3 board: the chip facts
(level 1), the detector framework, and the camera detector reading a known map
(level 2). Not yet run on hardware: the bounded scan (level 3), the microphone,
servo and motor tests, the `unverified` health state. Host tests cover the
framework and the pin table.

## Requirement

A device must be able to check itself. The user does not know how the hardware is
wired and must not have to. So the firmware either finds out what is there and
whether it works, or says plainly that it cannot tell. It does not wait to be
told a pin table, and it does not report "ok" for something it cannot observe.

This is a requirement on every dc-device device, not only on a camera.

## Three kinds of peripheral

| Class | How the device can know | Examples | Result it may report |
|---|---|---|---|
| A. Answers on a bus | Read an identity register or get an acknowledgement | camera (SCCB), IMU, I2C display or sensor, SPI sensor with a chip id | `ok` or `failed`, with the chip id |
| B. Shows an electrical effect | Drive or sample and watch a signal come back | microphone (samples vary), button (level changes when pressed), a sensor with an analog output, a motor driver with current sense | `ok` or `failed`; sometimes only with the user's help |
| C. No signal comes back | Nothing electrical reports it | LED, speaker, motor, servo, most displays | `unverified` until a person or an external sensor confirms |

Class C must never be reported as `ok` from software alone. The contract needs a
state for it (see "Contract changes").

## How the wiring is found

Four levels, tried in order, each only when the one before did not settle it.

1. **Chip facts.** Always available and always read-only: chip model and revision,
   flash size, PSRAM size, MAC. Cheap enough for `/diagnostics`.
2. **Known board maps, verified.** The firmware carries pin maps of boards it knows.
   A map is never trusted: the self-check confirms it by the class A or B test
   below. A map that fails verification is discarded and the result says so.
3. **Bounded scan.** For class A buses only: try pairs of pins from a set of safe
   pins as an I2C/SCCB bus and look for a known identity. Open drain with pull-ups,
   no push-pull output, so a pin that turns out to be wired to something else is
   not driven against it. Runs only inside an explicit `/self_test`, never at boot
   and never from `/status`.
4. **A person.** For class C and for anything the device could not settle: a
   self-test that asks the user to confirm ("did the LED blink?") and reports
   `unverified` until answered. The device cannot ask; the host application does.

Pins never offered to a scan: the flash and PSRAM pins (26 to 32 on an ESP32-S3,
33 to 37 when the PSRAM is octal), USB (19, 20), the console UART (43, 44) and the
strapping pins (0, 3, 45, 46). Which pins are safe is a property of the chip, kept
in one table, and is not guessed per board.

## Result

A self-check report says, per peripheral, one of:

- `ok`: observed working, with the evidence (for example "chip id 0x2642 at 0x30
  on GPIO 4/5").
- `failed`: expected and not found, or found and misbehaving.
- `unverified`: present as far as the device knows, but not observable.
- `not_found`: a known class A or B peripheral that was looked for and is absent.
  Not an error by itself; whether it matters is the profile's call.

The report includes the wiring that was found and how (`known_map` or `scan`), so
a host can show it and a developer can add it to the known maps.

## Contract changes (to decide)

- `/status` stays cheap and read-only. It re-checks only what a previous self-check
  already located, and reports "not yet discovered" as a warning, not a failure.
- `/self_test` runs discovery. It may take seconds (a bounded scan); the host must
  allow for it (suggested budget 10 s).
- The health result needs a fourth state besides ok / warning / failed for
  `unverified`. `docs/health-contract-v0.1.md` has `skipped`, which means
  "not run"; it is not the same thing.
- A profile can require a peripheral (class A or B must be `ok`; class C may be
  required to be `unverified` or better, because that is all software can say).
- Discovered wiring belongs in `/diagnostics` under a named key so a management UI
  can show it without parsing check messages.

## Servos and motors

Required parts of the self-check. They move, so they have rules the other tests do
not.

What software can and cannot know:

| Part | What the test does | Can software see the result? |
|---|---|---|
| Servo, no feedback | A small move (a few degrees) and back, then release | No. `unverified` until a person confirms it moved. |
| Servo with position feedback (analog or digital) | The same move, read the feedback | Yes (class B): `ok` if the reading follows. |
| DC motor with an encoder | A short, low-duty pulse in each direction | Yes: count encoder edges. `ok` if they count and the direction agrees. |
| DC motor with current sense | The same pulse | Partly: current above the idle level shows the driver and motor draw. It does not show the wheel turned. |
| DC motor with only a driver fault pin | The same pulse, read the fault pin | Only a fault is visible. No fault is not proof. `unverified`. |
| Stepper | A few steps and back | Only with a limit or feedback input. Otherwise `unverified`. |

Rules for a test that moves something:

- It uses the device's safe state and `max_duration_ms` (`safety-kernel-v0.1.md`).
  The test cannot leave a part driven: it ends by invoking the safe state, even
  when it fails halfway.
- Small and short: a servo move of at most 10 degrees, a motor pulse of at most
  300 ms at low duty. A test reports the amplitude it used.
- It runs only when asked for, never from `/status`, never at boot, never on a
  timer. This is the existing health-contract rule for actuating self tests.
- A person is assumed to be present and the machine free to move (a car on a
  stand). The device cannot check this. The host must tell the user before it
  runs the test.
- A test that moves a part with no feedback does not report `ok`. It reports
  `unverified` and the host asks the user.

Open: `POST /self_test` takes no arguments today, so one call runs every self
test, moving ones included. A car that drives off the desk because someone pressed
"self test" is not acceptable. Decide between (a) a way to run only the tests that
do not move anything, with moving tests marked and run only when named, and (b)
leaving it as is and keeping moving tests out of the default set. This is a
contract change and is not decided here.

## What this cannot do

- Find which of two identical-looking pins is which for a class C device.
- Tell a disconnected servo from a connected one.
- Find camera data lines by protocol. They carry no identity. The plan is to turn
  the sensor's own test pattern on and look for the pixel clock and sync signals
  by their edge rate, as an input-only measurement. That is a design idea, not
  something tried.
- Be safe on a board whose pins are connected to something unexpected and
  dangerous. The scan keeps to open-drain and input, which lowers that risk and
  does not remove it. This is why it only runs on an explicit request.

## Order of work

1. Chip facts in `/diagnostics` (no risk).
2. A detector interface in the ESP32 library: a detector names a peripheral, its
   class, the pins it may use, and a `check()` that returns a result with evidence.
3. Camera (SCCB identity) as the first detector, with the known-map and the scan.
4. Microphone by sample activity (class B, input only).
5. The `unverified` state in the contract, the SDK and the conformance check.
6. Servo and motor tests, after the moving-self-test question above is decided.
7. The data-line detection idea, only after the above works on hardware.

## Open items

- The fourth health state and its wire name.
- Whether the known maps live in the firmware or are supplied by the host.
- Whether a failed scan result is stored in NVS to avoid re-scanning.
- How a host asks the user for the class C confirmation (gap to the host).
