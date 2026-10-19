# Self-check v0.1

Status: draft. Implemented and run on three real ESP32-S3 boards of the same model:
the chip facts, the detector framework, an I2C detector that finds the buses with
no pin table (pairs among the pins something holds high first, then every safe
pair) and identifies the chips it knows by id, an input-only pin survey, and a
blind microphone search that confirms a candidate by playing a tone on the speaker
and listening for it. Blind runs: board 1 finds its camera (OV2640) on GPIO 4/5 and
two unrecognised devices (0x40, 0x70) on GPIO 47/48 and no microphone; board 3
finds the microphone (SCK 21, WS 40, SD 2) with no pin table and no camera; board 2
was run only with the first detector versions and not with the final microphone
search. A person was the observer for the speaker and the screen (boards 2 and 3; not
board 1). The board 1 result matches what the owner says (no microphone fitted);
the board 3 result matches the pins in its old program. Not yet run on hardware:
identification of any chip other than the camera (the table is tested only against
fake devices), servo and motor tests, camera-as-observer, the `unverified` health
state on the wire. Host tests cover the framework, the pin table, the id table and
the audio analysis; the I2S and GPIO code runs only on a board.

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

## Observers: one peripheral checks another

A peripheral with no signal of its own (class C) can still be checked when another
peripheral on the same board can observe its effect. This turns many "unverified"
into `ok`, with the observer named as evidence. None of it is built or tried yet.

| Peripheral | Observer | Test |
|---|---|---|
| Speaker | Microphone | Play a tone, look for that frequency in the samples. Checks both at once. |
| Microphone | Speaker | The same loop, from the other side. A microphone alone is checked by its samples varying. |
| Display, LED | Camera | Show or light a known pattern and compare frames. Works only if it is in the camera's view. |
| Servo, motor | Camera, or an encoder | Move a little and compare frames or count encoder edges. Same condition. |
| Button | A person | The device cannot press it; it waits for a press and reports it. |
| Wired pins | None needed | Read every safe pin as an input with a pull-up, then a pull-down. A pin that does not follow the pull is wired to something. Input only, drives nothing. Tells where to look, not what is there. |

Limits: an observer must exist and be able to see or hear the effect. A speaker in
a room with the microphone covered, an LED out of the camera's view or a servo
nothing watches stay `unverified`. The result names the observer, so a failed
loop is not blamed on the wrong part.

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

## Lessons from the first hardware runs

- The board's I2C lines use the chip's internal pull-ups (about 45 kOhm). A single
  probe sometimes got an acknowledge from nothing, at random addresses, on 2 of 6
  boots. A device now counts only after three probes in a row answer, and a known
  device is gone only after three in a row do not. 12 boots after the change: no
  phantom device.
- Building and tearing down a bus on every `/status` glitched the sensor and, after
  a probe timeout, hung the driver until the watchdog reset the chip. The bus stays
  open once a device was found, and a timed-out probe ends the attempt.
- Running a detector inside the HTTP task needs more than the default 4096-byte
  stack.
- A scan that finds nothing is reported as `not_found`, which degrades `/status`.
  Board 1 has no microphone and is therefore in the warning state for good, which is
  a false alarm for the user. A board that has no I2C device is the same. The profile
  would have to say which peripherals are expected; that is open.

- The first scan after boot cost 8.5 s and ended at its time budget before reaching
  the pair that held a real bus. A bus needs pull-ups, so the scan now tries the
  pairs among the pins that stay high with a pull-down on first (a few
  milliseconds) and searches all pairs only if no bus was found. Cost: a second bus
  with no external pull-ups is missed when another bus was found.
- Two detectors that touch pins must not undo each other: the pin survey reset the
  pins of an open I2C bus and broke it. Detectors now claim the pins they hold
  (`claim_pin`) and a survey skips claimed pins.
- A peripheral only a person can check (speaker, screen) is reported `unverified` with
  a `confirm` object: the capability that exercises it and the question to ask. The
  device cannot store the answer. Where a host keeps "the user confirmed this at this
  time", and for how long it stays valid, is not decided.
- Registering detectors in `app_main` overflowed the main task's default 3584-byte
  stack after the fifth one and caused a boot loop; the examples now set 8192.
- Reading the report after a Quick run hides what the Full run said (a Quick run
  replaces the rows). A scan cut short by its budget was invisible this way.

- **A detector needs a known positive.** The microphone search ran on three boards
  and reported "not found" three times before anyone checked it against a board
  that has a microphone. It could not have found one. The causes, found only once a
  known microphone was available: (1) the candidate filter waited 1.5 ms after
  starting the clocks, and the microphone sends nothing for the first tens of
  milliseconds (0 edges at 1.5 ms, 80 to 130 edges at 20 ms or more); (2) the
  judgement required the low 8 bits of every 32-bit word to be zero, and this
  microphone's are not. Every detector must be run against a part that is there and a
  part that is not before its "not found" is believed.
- **The speaker and screen rows are not detections.** The camera example lists them
  unconditionally, with pins taken from the board's old program, so they appear on
  every board running it, including one with no speaker or screen. They are
  `unverified` with a `confirm` question, which is honest about the verdict but not
  about presence. Turning them into real detections needs a way to tell what board it
  is, or an observer.
- **A line that moves is not a part.** The microphone is accepted as working only if
  it answers a known sound: a 880 Hz tone played on the speaker at the same time must
  show up in the samples at least 8 times stronger than in a quiet read. Without a
  way to play a sound the candidate is `unverified`, never `ok`. Cost: a full self
  test on a board that has a microphone makes a one second sound. A board with no
  candidate stays silent.
- **Peak memory matters on a board without PSRAM.** The camera example has about
  250 KB of heap. Reading two blocks of samples while the speaker task held a whole
  tone in memory made `new` fail, and with exceptions off that is an `abort()`. The
  samples are read in half-size blocks and the tone is written in chunks.
- **The same board gave different I2C results on different days.** Board 3 found no
  I2C device on its first run and found the pair at 0x40/0x70 on GPIO 47/48 on a
  later one, with the pins that were held low no longer held low. The detector code
  for I2C did not change between them. Whether something was connected or
  disconnected in between is not known.
- **A board dropped off the network once** (no ping, no HTTP) and rejoined at the
  same address after a reset. Not explained; it may be the same thing as the single
  empty replies noted in the test procedure.

## Open items

- The fourth health state and its wire name.
- Whether the known maps live in the firmware or are supplied by the host.
- Whether a failed scan result is stored in NVS to avoid re-scanning.
- How a host asks the user for the class C confirmation (gap to the host).
