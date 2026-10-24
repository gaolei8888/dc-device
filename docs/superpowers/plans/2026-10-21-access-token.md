# Access token (authentication) Implementation Plan

> **DROPPED by the owner on 2026-10-22 ("不要访问码").** Kept for the record only; the code is on the unmerged branch `feature/access-token` and must not be merged. Do not build from this.


> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Protected dc-device calls need a per-board access token that is handed out only over the USB cable.

**Architecture:** A portable `AccessControl` class (hash store, constant-time compare, lockout, token issue) is owned by the firmware and consulted by `Endpoint::handle`, which gets the `Authorization` header as a new input. The USB setup protocol gains `token`, `unpair` and `hello.paired` through new `SetupBackend` callbacks. The Python SDK gets the same check with the token as a constructor argument. All logic is host-tested; the board test comes last.

**Tech Stack:** C++17 (host tests via `tests/esp32/run.sh`, ESP-IDF 5.4 NVS and `esp_fill_random` on the board), Python 3 (pytest).

**Spec:** `docs/superpowers/specs/2026-10-20-access-token-design.md`

## Global Constraints

- Token: 32 random bytes, written as 43 characters of URL-safe base64 (no padding). Sent as `Authorization: Bearer <token>`.
- The board stores only a SHA-256 hash (NVS namespace `dcauth`); comparison is constant time.
- Protected: `POST /invoke`, `POST /self_test`, `GET /diagnostics`. Open: `GET /health`, `/identity`, `/manifest`, `/status`.
- Missing or wrong token: `401`, header `WWW-Authenticate: Bearer`, body `{"error":{"code":"Unauthorized","message":"..."}}`, same reply for missing and wrong. Release board with no token: `401`, code `Unpaired`. Unpaired development board: allowed.
- 10 consecutive failures -> every protected call answers `401` for the next 30 s without looking at the token. Counter is in RAM only, global, reset by a correct token; shown in `/diagnostics`, not in `/status`.
- `/identity` gains `"auth":{"required":bool,"paired":bool}`.
- USB: `token` (replaces any earlier token, answers once, never logged), `unpair`, `hello` gains `"paired"`. `forget_wifi` does not touch the token. Timeout 2 s.
- "Release" is `CONFIG_DC_DEVICE_RELEASE`. No button press for re-keying.
- The token must never appear in any HTTP reply, `/diagnostics`, or log line.
- Commits: code and docs separate; trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Commit only when the user asks; the commit steps below are the points where the user is asked.

## Review Focus

- `Authorization` header with wrong scheme (`Basic x`, `bearer x` in lower case, `Bearer` with no token, extra spaces): behave as a wrong token, never crash. (Task 2)
- Token that is a prefix of the real one, or the real one with one extra character: refused. (Task 2)
- Lockout while the correct token is presented: still refused until 30 s pass, then accepted. (Task 2)
- `token` twice in a row: the first token stops working immediately. (Task 3)
- Reboot: pairing survives (hash loaded from storage), failure counter resets. (Task 2, Task 5)
- `unpair` on an unpaired board answers `ok`. (Task 3)
- Storage failure during `token`: error `internal`, previous token still valid. (Task 3)

## File Structure

- Create `platforms/esp32/include/dc_device_sha256.hpp`, `src/dc_device_sha256.cpp` - portable SHA-256 (host has no mbedtls).
- Create `platforms/esp32/include/dc_device_access.hpp`, `src/dc_device_access.cpp` - `AccessControl`, token encoding.
- Create `platforms/esp32/include/dc_device_access_nvs.hpp`, `src/dc_device_access_nvs.cpp` - ESP-only: NVS storage and hardware random for `AccessControl`.
- Modify `platforms/esp32/{include,src}/dc_device_endpoint.*` - check, `/identity` auth field.
- Modify `platforms/esp32/src/dc_device_http_server.cpp` - pass header, send `WWW-Authenticate`.
- Modify `platforms/esp32/{include,src}/dc_device_setup.*` - `token`, `unpair`, `hello.paired`.
- Modify `examples/esp32/camera_board/main/main.cpp` and its `CMakeLists.txt` - wiring.
- Modify `tests/esp32/test_endpoint.cpp`, `tests/esp32/run.sh`.
- Modify `sdk/python/dc_device/{device.py,http_server.py,http_client.py}`, `tests/python/test_device.py` (or a new `tests/python/test_auth.py`).
- Modify docs: `docs/http-transport-v0.1.md`, `docs/usb-setup-protocol-v0.1.md`, `docs/contract-gaps-draft.md`, `schemas/device-identity.schema.json`.
- Follow-up, separate repo, not in this plan: `dc-device-sim` accepts a token.

---

### Task 1: Portable SHA-256

**Files:**
- Create: `platforms/esp32/include/dc_device_sha256.hpp`, `platforms/esp32/src/dc_device_sha256.cpp`
- Modify: `tests/esp32/run.sh` (add the .cpp), `tests/esp32/test_endpoint.cpp`

**Interfaces:**
- Produces: `namespace dc_device { std::array<uint8_t,32> sha256(const void* data, size_t length); }`

- [ ] **Step 1: Write the failing test** `test_sha256()` in `test_endpoint.cpp`, called from `main()`: sha256 of `""` is `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`; of `"abc"` is `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`; of 1,000,000 times `'a'` is `cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0`. Compare via a hex helper in the test.
- [ ] **Step 2: Run** `bash tests/esp32/run.sh` - Expected: compile failure, `dc_device_sha256.hpp` missing.
- [ ] **Step 3: Implement** `sha256` in `dc_device_sha256.cpp` (plain FIPS 180-4, no dependencies); add the file to `run.sh`.
- [ ] **Step 4: Run** `bash tests/esp32/run.sh` - Expected: "all tests passed".
- [ ] **Step 5: Commit** (when the user asks): `feat: portable SHA-256 for the access token`

---

### Task 2: AccessControl and the endpoint check

**Files:**
- Create: `platforms/esp32/include/dc_device_access.hpp`, `platforms/esp32/src/dc_device_access.cpp`
- Modify: `platforms/esp32/include/dc_device_endpoint.hpp`, `platforms/esp32/src/dc_device_endpoint.cpp`, `tests/esp32/run.sh`, `tests/esp32/test_endpoint.cpp`

**Interfaces:**
- Consumes: `sha256` (Task 1).
- Produces (`namespace dc_device::esp32`):
  ```cpp
  enum class AuthResult { Allowed, Unauthorized, Unpaired, LockedOut };
  struct AccessStorage {                       // all three may be empty in tests that do not persist
      std::function<bool(std::array<uint8_t,32>&)> load;   // false = nothing stored
      std::function<bool(const std::array<uint8_t,32>&)> save;
      std::function<bool()> erase;
  };
  class AccessControl {
  public:
      AccessControl(bool enforce_when_unpaired, AccessStorage storage,
                    std::function<void(uint8_t*, size_t)> random,
                    std::function<double()> clock);       // seconds, monotonic
      bool paired() const;
      bool required() const;                  // enforce_when_unpaired || paired()
      AuthResult check(const std::string& authorization_header);   // "" = none sent
      bool issue(std::string& token_out);     // new token; false = storage failed, old token kept
      bool revoke();                          // false = storage failed
      unsigned consecutive_failures() const;
      std::string diagnostics_json() const;   // {"paired":..,"consecutive_failures":..,"locked_out":..}
  };
  std::string token_encode(const uint8_t* bytes, size_t length);  // URL-safe base64, no padding
  ```
  `Endpoint` gains `void set_access_control(AccessControl* access)` (not owned, may be null = no checks) and `handle(method, target, body, const std::string& authorization = "")`. `Response` gains `std::string www_authenticate;` (empty unless 401).
- `AccessControl` loads the stored hash in its constructor and is safe to call from two tasks (internal `std::mutex`).

- [ ] **Step 1: Write failing tests** `test_access_control()` and `test_endpoint_auth()`:
  - `token_encode` of 32 bytes `0x00..0x1F` is 43 characters, only `[A-Za-z0-9_-]`; `issue` twice gives different tokens.
  - Unpaired, enforce=true: `check("")` -> `Unpaired`. Unpaired, enforce=false: `Allowed`.
  - After `issue(t)`: `check("Bearer "+t)` -> `Allowed`; `check("")`, `"Bearer "`, `"Bearer"`, `"bearer "+t`, `"Basic "+t`, `"Bearer "+t+"x"`, `"Bearer "+t.substr(0,42)`, `"Bearer  "+t` (two spaces) -> `Unauthorized`.
  - Second `issue` -> the first token now `Unauthorized`, the new one `Allowed`.
  - Lockout: 10 wrong tokens -> the 11th call with the correct token is `LockedOut`; advance the injected clock by 29.9 s -> still `LockedOut`; by 30.1 s -> `Allowed` and `consecutive_failures()==0`. 9 wrong then a correct one resets the count.
  - Storage: a fake storage; a new `AccessControl` over the same storage is paired and accepts the old token (reboot); `save` returning false makes `issue` return false and the old token still `Allowed`; `revoke` clears and `paired()` is false; `revoke` unpaired returns true.
  - `diagnostics_json()` never contains the token or its hash (search for both in the string).
  - Endpoint: with enforce=true and a paired token, each of `POST /invoke`, `POST /self_test`, `GET /diagnostics` without header -> 401 with `www_authenticate=="Bearer"` and body exactly `{"error":{"code":"Unauthorized","message":...}}`; `/health /identity /manifest /status` -> 200 without header; with the header all protected ones pass (invoke still returns its normal result); unpaired+enforce -> `Unpaired` code; unpaired dev -> allowed; locked out -> 401 `Unauthorized`; the 401 body never contains the token; 404 for unknown paths stays 404 without a header.
  - Open endpoints ignore the header: 20 calls to `/status`, `/identity`, `/health`, `/manifest` with a wrong `Authorization` all answer 200, `consecutive_failures()` stays 0, and the next protected call with the right token is `Allowed` (Python SDK test: same).
  - `/identity`: `auth` present as `"auth":{"required":true,"paired":false}` etc. for the four states; with no `AccessControl` set, `identity()` output is unchanged (existing `test_identity` still passes byte for byte).
- [ ] **Step 2: Run** `bash tests/esp32/run.sh` - Expected: compile failure (missing header).
- [ ] **Step 3: Implement** `AccessControl` in `dc_device_access.cpp` (compare the 32-byte hashes with a constant-time loop, not `memcmp`; parse the header as exactly `"Bearer "` followed by the token) and add the file to `run.sh`.
- [ ] **Step 4: Implement** the check in `Endpoint::handle` before dispatching to the three protected routes only (open routes and 404 are untouched); map `AuthResult` to responses per Global Constraints; add `auth` to `Endpoint::identity()` only when an `AccessControl` is set, as the last field, before the closing brace.
- [ ] **Step 5: Run** `bash tests/esp32/run.sh` - Expected: "all tests passed".
- [ ] **Step 6: Commit** (when the user asks): `feat: access token check in the endpoint`

---

### Task 3: USB `token`, `unpair`, `hello.paired`

**Files:**
- Modify: `platforms/esp32/include/dc_device_setup.hpp`, `platforms/esp32/src/dc_device_setup.cpp`, `tests/esp32/test_endpoint.cpp` (`SetupRig` and `test_setup_protocol`)

**Interfaces:**
- Consumes: nothing from earlier tasks directly; the firmware wires `AccessControl` in Task 5.
- Produces: `SetupBackend` gains
  ```cpp
  std::function<bool()> paired;                      // empty = reports false
  std::function<bool(std::string& token_out)> issue_token;   // false = storage failed
  std::function<bool()> unpair;
  ```
  Replies: `{"v":1,"id":N,"ok":true,"token":"..."}`; `unpair` -> `{"v":1,"id":N,"ok":true,"paired":false}`; `hello` gets `"paired":true|false` placed right after `"serial"`.

- [ ] **Step 1: Write failing tests** (extend `SetupRig` with fake `issue_token` returning a counter-based token and a switchable failure):
  - `token` -> the exact reply line with the token; a second `token` -> a different token; with the fake failing -> `{"v":1,"id":N,"ok":false,"error":{"code":"internal",...}}` (use the existing error shape) and the previous fake token unchanged.
  - `token` with no `issue_token` callback -> `unknown_command` is wrong now; expect `internal`.
  - `unpair` -> ok; `hello.paired` is true after `token`, false after `unpair`; `unpair` when unpaired -> ok.
  - `forget_wifi` leaves `paired` unchanged.
  - The token string never appears in any other output line of the rig (hello, errors) and is not echoed by `write` for unrelated commands.
  - Update the existing `hello` expected strings to include `"paired":false`.
- [ ] **Step 2: Run** `bash tests/esp32/run.sh` - Expected: FAIL on the new checks.
- [ ] **Step 3: Implement** `token` and `unpair` in `SetupProtocol::handle_line` dispatch and add `paired` to `hello`.
- [ ] **Step 4: Run** `bash tests/esp32/run.sh` - Expected: "all tests passed".
- [ ] **Step 5: Commit** (when the user asks): `feat: token, unpair and hello.paired in the USB setup protocol`

---

### Task 4: HTTP server passes the header

**Files:**
- Modify: `platforms/esp32/src/dc_device_http_server.cpp`

**Interfaces:**
- Consumes: `Endpoint::handle(..., authorization)` and `Response::www_authenticate` (Task 2).

- [ ] **Step 1: Implement** in `dispatch`: read `Authorization` with `httpd_req_get_hdr_value_len` / `httpd_req_get_hdr_value_str` into a `std::string` (cap at 128 bytes; a longer value is passed as an empty string, which is refused like a wrong token); pass it to `handle`; in `send`'s caller set `WWW-Authenticate` when `response.www_authenticate` is non-empty. Check that `CONFIG_HTTPD_MAX_REQ_HDR_LEN` (default 512) fits.
- [ ] **Step 2: Verify the build** with the release-build container: `bash scripts/build-release.sh` is the full path; for a quick check build `examples/esp32/camera_board` the way `~/esp32-build/camera_board_setup` was built. Expected: compiles with no warnings from this file.
- [ ] **Step 3: Commit** with Task 5 (it cannot be tested without the wiring).

---

### Task 5: Firmware storage, wiring and the camera_board

**Files:**
- Create: `platforms/esp32/include/dc_device_access_nvs.hpp`, `platforms/esp32/src/dc_device_access_nvs.cpp`
- Modify: `examples/esp32/camera_board/main/main.cpp`, `examples/esp32/camera_board/main/CMakeLists.txt` (add the new source, `esp_hw_support` if needed)

**Interfaces:**
- Consumes: `AccessControl`, `AccessStorage` (Task 2); `SetupBackend` fields (Task 3).
- Produces (ESP only): `AccessStorage make_nvs_access_storage();` (namespace `dcauth`, key `hash`, 32-byte blob); `void hardware_random(uint8_t*, size_t);` (wraps `esp_fill_random`).

- [ ] **Step 1: Implement** `make_nvs_access_storage` and `hardware_random` (the file is wrapped in `#ifdef ESP_PLATFORM` like its neighbours).
- [ ] **Step 2: Wire** in `main.cpp`: one static `AccessControl access(CONFIG_DC_DEVICE_RELEASE, make_nvs_access_storage(), hardware_random, <steady clock seconds>)`; `endpoint.set_access_control(&access)`; `endpoint.add_diagnostic("auth", ...)` returning `access.diagnostics_json()`; fill the three new `SetupBackend` callbacks from `access`.
- [ ] **Step 3: Verify** the dev build compiles, and the release build (`bash scripts/build-release.sh`) compiles and still reports `CONFIG_DC_DEVICE_RELEASE=y`.
- [ ] **Step 4: Commit** (when the user asks): `feat: access token on camera_board, stored in NVS`

---

### Task 6: Python SDK

**Files:**
- Modify: `sdk/python/dc_device/device.py`, `sdk/python/dc_device/http_server.py`, `sdk/python/dc_device/http_client.py`
- Test: `tests/python/test_auth.py` (new)

**Interfaces:**
- Produces: `Device(..., access_token: str | None = None)`; when set, `Device.identity()` includes `"auth":{"required":True,"paired":True}`, when `None` it includes `"auth":{"required":False,"paired":False}`. `make_server`/`serve` enforce the token on `/invoke`, `/self_test`, `/diagnostics` with the same 401 bodies and headers as Task 2 (lockout rule too). `DeviceHttpClient(base_url, token: str | None = None)` sends `Authorization: Bearer <token>` on every request when given.

- [ ] **Step 1: Write failing tests** (start `make_server` on port 0 in a thread, use `curl`-free `http.client`): no token configured -> everything open and `identity.auth == {"required":False,"paired":False}`; with a token: the same matrix as Task 2 (open routes 200, protected 401 without / with wrong / with `Basic`; right token 200), 401 body shape and `WWW-Authenticate: Bearer`, lockout after 10 wrong then 401 for the correct token until a monkeypatched clock moves 30 s, `DeviceHttpClient(token=...)` works and one without gets `HTTPError` 401; the existing identity tests keep passing (`auth` is additive; check `tests/python/test_identity_profile.py` against `schemas/device-identity.schema.json`).
- [ ] **Step 2: Run** `PYTHONPATH=sdk/python $P -m pytest -q tests/python/test_auth.py` (venv python in the scratchpad) - Expected: FAIL.
- [ ] **Step 3: Implement** with a small `_AccessGate` in `http_server.py` shared by the handler class; compare with `hmac.compare_digest` on SHA-256 digests.
- [ ] **Step 4: Run** `PYTHONPATH=sdk/python $P -m pytest -q tests/python` - Expected: all pass.
- [ ] **Step 5: Commit** (when the user asks): `feat: access token in the Python SDK`

---

### Task 7: Contract text and schema

**Files:**
- Modify: `docs/http-transport-v0.1.md` (Security section), `docs/usb-setup-protocol-v0.1.md` (replace the pending `token` text; add `unpair`, `hello.paired`; fix the "not implemented" lines at the top and bottom), `docs/contract-gaps-draft.md` (gap 15 and 16 status), `schemas/device-identity.schema.json` (describe `auth`), `docs/esp32-platform.md` if it mentions pairing.

- [ ] **Step 1: Write** the text from the spec sections "The scheme", "Replies", "USB setup protocol", including the cleartext limitation and the lockout caveat. Gap 15: partly closed (token); gap 16: closed for the cable pairing.
- [ ] **Step 2: Run** `PYTHONPATH=sdk/python $P -m pytest -q tests/python` - Expected: still passes with the schema edit.
- [ ] **Step 3: Commit** (when the user asks): `docs: access token contract`

---

### Task 8: Board test (development board 1)

**Files:** scratchpad script only (`.../scratchpad/auth_flow.py`), not committed.

- [ ] **Step 1:** Build a development image with the Wi-Fi known map, flash to board 1 (`/dev/cu.usbmodem5ABA0145631`) the usual way. Ask the user before flashing.
- [ ] **Step 2:** Unpaired dev board: `/invoke heap.read` works; `/identity` shows `required:false,paired:false`.
- [ ] **Step 3:** Over USB send `token` (wait 2 s after opening the port); with that token: `/invoke` 200, `/diagnostics` 200 (and its `auth` block has no token); without header or with a wrong one: 401 and `/invoke` did nothing; 10 wrong tries then the right token is refused for ~30 s and then accepted.
- [ ] **Step 4:** Send `token` again: the first token gets 401, the second works. Power-cycle: the second still works, the failure counter is 0.
- [ ] **Step 5:** `unpair`: calls open again, `hello.paired` false. Capture all console output of the session and check the token string appears only in the one `token` reply.
- [ ] **Step 6:** Build the release package (`scripts/build-release.sh`) and confirm from a dirty-false build that an unpaired release image refuses `/invoke` with `Unpaired`. Do not flash it without the user's approval.
- [ ] **Step 7:** Report results to the user, then tell DC Desktop the contract (Bearer header, 401 handling, store the token in the OS secret store).

---

## Self-review

- Spec coverage: scheme/replies/lockout (T2), identity field (T2, T6, T7), USB commands (T3), where the check lives (T2, T4), Python SDK (T6), simulator (follow-up, other repo), contract text (T7), board test (T8), release vs dev (T2 `enforce_when_unpaired`, T5 wiring, T8 step 6).
- Types: `AccessControl`, `AccessStorage`, `AuthResult`, `Response::www_authenticate`, `SetupBackend::{paired,issue_token,unpair}` are named identically in every task.
- Not covered on purpose: HTTPS, OTA calls (they reuse the check later), `dc-device-sim`.
