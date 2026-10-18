# Self-check test procedure v0.1

Status: draft. This is the procedure that was actually run on two ESP32-S3 boards of
the same model (the "first" and "second" board below) to test the self-check in
`self-check-v0.1.md`. Every command was run on macOS with the ESP-IDF Docker image.
Where the procedure says something was not tried, it was not.

Goal of the procedure: find out what a board has, without telling the firmware (or the
tester) how it is wired, and separate three things that are easy to confuse:

- what the device **measured by itself**,
- what a **person** saw or heard,
- what was only **read from the board's old program** (a hint, never evidence).

## Rules

1. **Back up before overwriting.** A flash replaces whatever is on the board.
2. **Flash only with the owner's approval**, each time the firmware changes in a way
   that matters. Approval for one board or one firmware is not approval for another.
3. **Nothing that moves, nothing that makes a sound, nothing that drives unknown pins
   without saying so first.** A test that makes a sound or lights the screen needs a
   person present, who is told before it runs.
4. **Secrets stay out of the repository.** The Wi-Fi name and password live in a file
   outside it. A firmware image and a flash backup both contain that password.
5. **Report `ok` only for what was observed.** What could not be observed is
   `unverified`; what was looked for and not seen is `not_found`.

## What is needed

- One or more ESP32-S3 boards and a USB cable that carries data (a charge-only cable
  makes the board invisible: this happened once).
- `esptool` on the host. ESP-IDF is **not** installed on the host: the firmware is
  built in the Docker image `espressif/idf:release-v5.4` (about 15 GB).
- A 2.4 GHz Wi-Fi network. The settings go in `~/.config/esp32-build/bringup/wifi.conf`
  (mode 600):
  ```
  CONFIG_DC_DEVICE_WIFI_SSID="name"
  CONFIG_DC_DEVICE_WIFI_PASSWORD="password"
  ```
- Build output and `sdkconfig` are kept outside the repository, under
  `~/esp32-build/<example>/`.

## 1. Identify the board (read-only)

1. List the serial ports before and after plugging the board in:
   `ls /dev/cu.usb*`. A new `/dev/cu.usbmodem...` is the board. The USB chip can be read
   with `ioreg -p IOUSB -l -w0`. If no port appears, change the cable and the port.
2. Read the chip: `esptool --port <port> chip-id` and `flash-id`. This resets the board
   and interrupts whatever it runs. Only do it when nothing connected to it can move.
3. Expected for both boards: ESP32-S3 (QFN56) revision v0.2, 8 MB PSRAM, 16 MB quad
   flash (manufacturer `ef`, device `4018`), 40 MHz crystal. They differ only in the
   last byte of the MAC (`...67:40` and `...67:f0`).

## 2. Back up the flash (read-only)

1. Read the 16 MB in sixteen pieces of 1 MB, retrying a piece at a lower baud rate if
   it fails (a single 16 MB read failed twice with "Invalid head of packet"). Join the
   pieces into one file.
2. Verify against the chip: `esptool verify-flash 0x0 <file>` must print
   "Verification successful (digest matched)".
3. Record the size (16,777,216 bytes) and the sha256. Store it outside the repository.
4. Restoring: `esptool write-flash 0x0 <file>`. This was never needed; it is the reason
   the backup exists.

## 3. Read the old program (a hint only)

The backup holds the board's original MicroPython program and its filesystem (littlefs
at `0x200000`). Reading its pin definitions is useful to cross-check a result and is
the only source for the speaker and screen pins used in step 11. It is **not** how the
firmware finds anything, and a pin written in an old script does not prove a part is
fitted. Do not open its Wi-Fi settings file.

## 4. Build

From the repository root, for the `camera_board` example:

```
docker run --rm -v "$PWD":/project -v ~/esp32-build/camera_board:/out \
  -v ~/.config/esp32-build/bringup:/cfg:ro \
  -w /project/examples/esp32/camera_board espressif/idf:release-v5.4 \
  idf.py -B /out/build -DIDF_TARGET=esp32s3 -DSDKCONFIG=/out/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;/cfg/wifi.conf" build
```

- **Pass `-DIDF_TARGET=esp32s3`.** Without it the image is built for the original
  ESP32 and will not boot. Check that `build/flash_args` begins with
  `0x0 bootloader/bootloader.bin`.
- For a firmware that is not given the known camera pins, add a defaults file
  containing `CONFIG_DC_DEVICE_CAMERA_KNOWN_MAP=n` and use a separate build
  directory (delete its `sdkconfig` first so the option is regenerated).

## 5. Flash and find the board

1. `esptool --port <port> --baud 460800 --chip esp32s3 write-flash @flash_args`
   (run in the `build` directory). Both "Hash of data verified" lines must appear.
2. The firmware joins Wi-Fi and serves HTTP on port 8787. The address is assigned by
   the router and changes between boots. Find a board by its serial number (the MAC
   without separators) by asking every address on the subnet:
   `GET /identity` and compare `serial`.
3. Use `curl`, not Python, to reach the board from this host. On macOS an app without
   permission for "Local Network" gets "No route to host" while `ping` and `curl`
   work. (A desktop application showed this same symptom.)

## 6. Level 1: chip facts

`GET /diagnostics` returns `info.chip` (model, revision, cores, features, flash size,
MAC, IDF version, reset reason). Compare each value with step 1. `psram_bytes` is
`null` when the firmware does not enable PSRAM: that means "cannot tell", not zero.

## 7. Levels 2 and 3: find the I2C devices without a pin table

1. `GET /status` first. With no known pins the `peripherals` check is a warning
   (`PERIPHERAL_NOT_FOUND`, "run the self test to search"). That is correct.
2. `POST /self_test`. It looks first at the pairs among the pins that something holds
   high (a bus needs pull-ups), then at every safe pin pair if no bus was found. It
   takes about a second.
3. Read `GET /diagnostics`, `info.peripherals`. Each row has the peripheral, its class,
   a verdict, the evidence, the wiring found and how (`known_map` or `scan`).
4. **Read the report knowing that a later `/status` replaces it.** `/diagnostics` runs
   the cheap checks, which re-check and replace the rows. A search that was cut short
   by its time budget is visible only in the response of the run that was cut short.
5. The pin survey row lists pins held high, held low, and those that follow the pull.
   It skips pins an open bus holds.

Result on the two boards:

| | first board | second board |
|---|---|---|
| camera | found, OV2640, chip id 0x26 0x42, SDA 4 / SCL 5 | not found |
| devices on GPIO 47/48 | two, 0x40 and 0x70, not recognised | the same two |
| pins 4 and 5 | held high (the camera module pulls them up) | follow the pull |
| pin 15 | held low | follows the pull |

The difference between the boards is the camera module, shown by three separate
observations (an id, the pull-ups, pin 15).

## 8. Repeatability

Reset the board with an RTS pulse over the serial port, wait for HTTP, run the first
`POST /self_test`, read `/diagnostics`. Repeat 12 times per board. Pass: the same
peripherals every time, each first self test in under a second, no
`PERIPHERAL_FAILED`. Do this after any change to a detector: a result that is right on
the second run and wrong on the first is a bug (it happened twice).

## 9. A control for "not found"

"Not found" can mean the part is absent or the detector is broken. Run the same
firmware on a board known to have the part. The camera on the first board was the
control for the second: it was found by the same firmware.

## 10. Microphone (blind search, then a trial on given pins)

1. The self test searches pairs of floating pins: it drives a bit clock and a word
   clock on a pair at the weakest drive strength while sampling the other floating pins
   as inputs, then reads the pins that move as an I2S input and checks the samples. A
   microphone sends clean 24-bit words; a floating pin sends random bits.
2. It runs only in a full self test and carries on from where it stopped if its 8 s
   budget ends. Two runs cover about 500 pairs.
3. To separate "absent" from "not working", `debug.mic_try` (`sck`, `ws`, `sd`) reads
   an I2S input on the given pins and returns the sample statistics and which other
   floating pins move with the clock.
4. Result on both boards: no microphone found, and none at the pins in the old
   program. Reported `not_found`, not "absent": a microphone that needs an enable step
   the firmware does not know cannot be told from a missing one.

## 11. Speaker and screen (a person is the observer)

Neither can be observed by the board. Tell the person before each test.

1. `POST /invoke` `speaker.beep`: one 880 Hz tone of 0.3 s at about 6 % of full scale.
2. `POST /invoke` `screen.fill_test`: red, green, blue, black, about 0.7 s each.
3. Ask what was heard and seen, including whether the colours were right (red shown as
   blue means the colour order is wrong) and whether the picture was clean.
4. Record the answer as **human-confirmed**. The device does not know the answer and
   reports these as unverified. Both were confirmed on the second board; the colour
   order was not separately checked.
5. Pins come from the old program, so these two are not blind.
6. The self test now lists both as `unverified`, with the wiring and its source
   (`known_map`, meaning taken from a table) and a `confirm` object: the capability to
   run and the question to ask. The host runs the capability, asks, and keeps the
   answer; the device does not.

## 12. Stability and crashes

- Send 200 to 300 `GET /status` calls in a row. Pass: every answer is complete, the
  uptime keeps growing and `reset_reason` stays `poweron`. A `watchdog` reset means a
  detector blocked or hung.
- To see a crash, read the serial port while the request runs and keep the lines
  matching `abort|panic|Guru|wdt|rst:|E (`. Decode a backtrace with
  `xtensa-esp32s3-elf-addr2line -pfiaC -e build/<project>.elf <addresses>` inside the
  Docker image.
- Single empty HTTP replies were seen several times (about 5 % in one run, none in
  300 calls in another) and are **not explained**. Record them; do not average them
  away.

## 13. Recording a result

For every peripheral write one of: `ok` (the device observed it, with the evidence),
`failed`, `unverified`, `not_found`, and who observed it (device, person, or old
program). Do not write "absent" for `not_found`. Do not write `ok` for something only
a person saw.

## Pitfalls met while running this

- zsh: a loop variable named `path` is tied to `PATH` and breaks every command after
  it. Variables are not word-split, so `set -- $string` does nothing. Associative
  arrays and `declare -A` differ from bash.
- The serial port can be busy if a previous script still holds it.
- Several first runs after boot gave a different answer from later runs. Causes found:
  a scan budget that ended before the bus was reached, pins reset by one detector that
  another detector needed, and a bus built and torn down on every status call.
- The HTTP task stack of 4096 bytes overflowed inside a driver call; it is now 8192.
- The main task (where `app_main` runs) has 3584 bytes by default. Registering two
  more detectors there overflowed it and the board rebooted in a loop, each boot
  ending in a stack-overflow abort, and left the network. `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`
  is now in every example's `sdkconfig.defaults`. A board in a boot loop is recovered
  by flashing again over the serial port. Decode the backtrace to see which hook
  fired (`vApplicationStackOverflowHook`).
- The gpio driver logs one line per pin it configures. A search that configures
  thousands of pins spent most of its time printing at 115200 baud.
- A single I2C probe sometimes gets an acknowledge from nothing. A device counts only
  after three probes in a row answer.

## What this procedure does not cover

- Chips other than the camera sensor: the id table is tested only against fake
  devices, because no board had another identifiable chip.
- What the two devices at 0x40 and 0x70 are.
- Servos and motors (no board had any connected; the way a moving self test is
  requested is undecided).
- A microphone that needs enabling, a speaker or screen with no person present, and
  the camera as an observer.
- Boards of another model, and chips other than the ESP32-S3.
