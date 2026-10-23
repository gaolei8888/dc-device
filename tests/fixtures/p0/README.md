# P0 fixtures (sanitized)

Fixed samples for DCD and the hardware workbench to test against. Each file holds
`source`, `request`, `status` (HTTP status; null for the USB `hello`) and `body`.

- All except `hello.json` were read from a real board (tail 6740, firmware `0.1.0+deae116`,
  which is `main` at the time of writing) with requests that change nothing, on 2026-10-22.
  The MAC in `serial` is replaced by `AABBCCDDEEFF`; nothing else was altered.
  `free_heap_bytes` is the value that board returned.
- `hello.json` was **not** captured: it is built from the host protocol test. Replace it with a
  real capture once the user approves opening a board's serial port.
- This firmware has no access token (the owner decided against one on 2026-10-22): every call is
  open to the local network.
- The manifest lists only what the firmware has evidence is on the board. This board has a
  camera and no speaker or screen, so its manifest is just `heap.read`, and
  `invoke_undeclared_capability.json` shows `speaker.beep` refused like an unknown name.
  When a person declares a part over USB (`declare`), or the self-test observes it, its
  capabilities appear (`docs/self-check-v0.1.md`, "Declaring capabilities").
- The camera is detected (`/diagnostics` shows `camera: ok`) but there is no capture capability yet.
