# Motor and servo ports - draft design

Status: DRAFT for the owner, 2026-10-21. Nothing here is built, no hardware model is
assumed. It exists to list what the owner must answer. Boards are named by serial tail:
**6740** = camera only (no screen, no speaker, no microphone); **6718** = screen and
speaker (microphone per the 8 August record). Pin facts come from
`dc_classeditor/docs/hardware/pin-definition.py` and the ESP32-S3 pin rules; the
pin-definition file has not been verified on either board by this repo.

## Intent

Let an AI move a servo or run a motor that a user has wired to a board, without letting
the AI or the user choose arbitrary pins, and without a runaway output.

## Depends on

1. The access token (`2026-10-20-access-token-design.md`). Declaring "a motor is
   attached" changes what the board will do; it must not be possible from an
   unauthenticated request. Motion capabilities must not ship before the token exists.
2. The safety kernel (`docs/safety-kernel-v0.1.md`): every motion capability needs
   `max_duration_ms`, an emergency capability and a safe state; a self test answers 409
   while one runs.
3. Honest manifest (capabilities declared only for hardware that is there).

## Fixed ports (the firmware decides the pins)

The ports are a per-model table compiled into the firmware. Neither the AI nor the user
names a pin. Candidate pins = the safe set in `dc_device_pins.cpp` (1, 2, 4-18, 21,
38-42, 47, 48) minus every pin in the pin definition (screen 1/3/14/46, speaker
39/41/42, microphone 2/21/40, camera 4-13/16-18):

| Pin | Why it is left | Risk |
|---|---|---|
| GPIO 15 | no function in the pin definition | none known; also the 32 kHz crystal pad on some boards |
| GPIO 38 | no function in the pin definition | none known; some dev boards put an LED on it |
| GPIO 47 | no function in the pin definition | none known |
| GPIO 48 | no function in the pin definition | many dev boards put an RGB LED on it |

Left out on purpose: 0, 3, 45, 46 (strapping: wrong level at reset changes how the chip
boots), 19, 20 (USB), 26-37 (flash and PSRAM; 6740 has 8 MB PSRAM), 43, 44 (console).
Pins 1, 2, 14, 21, 39-42 are used on 6718 and are *physically unused* on 6740, which has
no screen, speaker or microphone - but whether they reach a header on 6740 is unknown
(question 3), so they are not offered.

Four pins is the whole budget on both boards: a servo needs 1, a DC motor through an
H-bridge needs 2, so e.g. two servos and one motor, or four servos. The table can be
widened once the owner shows where wires can be attached.

## Capabilities (once a port is confirmed)

- `servo.set_angle` `{port, angle_deg, speed_dps?}`: angle range and maximum speed
  per servo model; `max_rate_hz`; kind `action`; needs confirmation card every time.
  Holds the angle (a servo holds by itself); safe state = move to the neutral angle,
  or release the signal, per model (owner's choice, question 6).
- `motor.run` `{port, direction, speed_pct, duration_ms}`: `duration_ms` is required and
  capped by `max_duration_ms`; the watchdog (`dc_tick`) stops the motor at the limit
  whatever the caller does. Safe state = both inputs low (coast) or brake, per driver.
- `motor.stop` (emergency, no arguments): the device's single safe state
  (`safety_kernel.safe_state` in the manifest, as the safety kernel requires one) for
  every declared port: motors stop, servos go to their safe position. Named `motor.stop`
  because DC's "stop everything" button already looks for that name; a host should
  prefer reading `safety_kernel.safe_state` and the `emergency` flag from the manifest
  over a fixed name. It is applied at boot (`safe_on_boot`), so a reset never leaves an
  output driven.
- A servo has no "stop": its safe position is chosen per declared servo (neutral, or
  "stop sending the signal and let it go slack"), default neutral (question 6). No second
  emergency capability is added, so DC's list needs no new name.
- Outputs are LEDC/PWM only on the port pins; no raw gpio/spi/i2c capability exists.
- Not declared: anything for a port nobody confirmed.

## "I connected this" (declaration)

Motors and servos cannot be detected, so the user states what is wired. Proposal:

- A protected call `PUT /ports` (needs the token) with
  `{"ports":[{"port":"P1","kind":"servo","model":"<id from the board's list>"}]}`,
  stored in the board's NVS and applied at boot. Only models in a list compiled into
  the firmware are accepted (each model carries its limits); an unknown model is
  refused, there is no free-form entry.
- `GET /ports` (open) lists the ports, what is confirmed and what the board's table
  offers, so the host can draw the setup page.
- The manifest then declares the motion capabilities for confirmed ports only, and says
  `confirmed_by: "user"` (a new field, so a host can tell this apart from a detected
  part). The self test reports these ports as `unverified` ("declared by the user, cannot
  be observed"), never `ok`.
- What DC needs: a setup step (device page or pairing) that shows the plain-language
  power warning below, asks the user to pick the port and model, then makes the call with
  the token; keep the confirmation card on every motion call.

## Starter parts (the owner does not know the models; ordinary users will not either)

Designed for the commonest beginner parts, with conservative limits; the user picks the
option that looks most like what they bought, never types a model:

| Option shown to the user | Meaning | Pins | Limits (low on purpose) |
|---|---|---|---|
| "小舵机(SG90 / MG90S 一类,三根线)" | hobby servo, 50 Hz PWM, pulse 0.5-2.5 ms | 1 | angle 0-180 but pulse clamped to 0.6-2.4 ms (never driven to the mechanical stops), speed cap 120 deg/s |
| "电机 + 驱动板,方向脚+速度脚(TB6612 / L298N 一类)" | one direction pin + one PWM pin | 2 | PWM 20 kHz, speed cap 60 % until the user raises it, `duration_ms` cap 5000 |
| "不确定" | treated as the most limited option above (servo clamp, motor 30 %) | as chosen | lowest limits |

Correction to the "two pins" assumption: boards named TB6612 and L298N usually have two
direction inputs *and* one PWM input per motor (three signals); DRV8833 takes two PWM
inputs. The design therefore offers the two-signal wiring only (one direction + one PWM,
the other direction input tied low or high by a wire on the driver board). With four free
pins the budget is: one motor + two servos, or four servos, not two motors. Whether the
user can be told how to tie the second input is a page-text question for DC.

## Power and wiring (text for ordinary users)

Draft wording, plain Chinese for the page:

> 舵机和电机不能用开发板的 3.3V 供电,也不能靠电脑 USB 带动。请给它们单独接一个电源
> (舵机一般 5V,电机按它的说明书),并且把这个电源的"负极"和开发板的 GND 接在一起(共地),
> 否则它们收不到信号。电机不能直接接到开发板的针脚上,必须经过电机驱动板。接错可能烧坏
> 开发板或者电脑的 USB 口。接好线之前请先断开所有电源;不确定时先别接,找人看一下。

Risks to state in the design: motor current spikes resetting or damaging the board
(use the driver board and a separate supply); missing common ground makes outputs
random; a servo stalled against a stop overheats; a motor driven by a pin directly can
destroy the pin. A physical power switch on the external supply is the real emergency
stop; the software stop cannot cut power.

## When

Not in the first round. Order: token -> honest manifest -> (camera / screen / audio per
the owner) -> ports. Ports need hardware to test and a decided list of models, so no
estimate until the answers below exist.

## Questions only the owner can answer

1. Which servo and which motor models (and the motor driver board)? Voltage and current?
2. How many of each, on which board (6718, 6740, or both)?
3. Where can wires be attached on each board - a photo of the headers, or the model name
   of the carrier board? Do pins 1, 2, 14, 21, 39-42 reach a header on 6740?
4. Is there an LED on GPIO 38 or 48 on either board?
5. Will the external supply have a power switch within reach?
6. At emergency stop, should a servo go to neutral or just stop sending the signal; should
   a motor coast or brake?
7. Is the "I connected this" step done at pairing or on the device page (affects the DC
   work, not the firmware)?
