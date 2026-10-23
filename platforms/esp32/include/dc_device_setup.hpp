#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dc_device_json.hpp"

namespace dc_device::esp32 {

// USB setup protocol v0.1 (docs/usb-setup-protocol-v0.1.md): the host gives the
// device Wi-Fi credentials over the console serial port. This file is the
// protocol only - framing, parsing, commands and error codes. Where the Wi-Fi
// actually happens is behind SetupBackend, so all of it runs in host tests.

enum class WifiState { Unconfigured, Connecting, Connected, Offline };
const char* wifi_state_name(WifiState state);

// What `hello` says about the device. Same values as GET /identity.
struct SetupInfo {
    std::string device_id;
    std::string type;
    std::string model;
    std::string firmware_version;
    std::string serial;
    std::string profile_id;       // empty = no profile
    std::string profile_version;
};

struct WifiOutcome {
    bool ok = false;
    // Set when !ok: wifi_not_found, wifi_auth_failed, wifi_no_address,
    // wifi_timeout, or internal.
    std::string code;
    std::string message;
    std::string ip;  // set when ok
};

enum class PartResult { Ok, UnknownPart, Failed };

struct SetupBackend {
    std::function<WifiState()> state;
    std::function<std::string()> ip;  // current address; empty if none
    uint16_t port = 8787;
    // Starts joining a network and returns at once. Calls `progress` with
    // "connecting" or "getting_address" while it works, and `done` exactly once,
    // within 20 s, from any task. It must write the credentials to storage only
    // if it succeeded, and keep the old ones otherwise.
    std::function<void(const std::string& ssid, const std::string& password,
                       std::function<void(const std::string& stage)> progress,
                       std::function<void(const WifiOutcome&)> done)>
        start_wifi;
    // Erases stored credentials and stops serving HTTP. Returns false on failure.
    std::function<bool()> forget_wifi;
    // Parts that nothing on the board can observe and that a person can declare present
    // (docs/self-check-v0.1.md, "Declaring capabilities"). Empty = none: `parts` answers an
    // empty list and `declare` answers unknown_part.
    std::function<std::vector<std::pair<std::string, bool>>()> parts;  // name, declared
    std::function<PartResult(const std::string& part, bool present)> declare_part;
};

constexpr size_t kSetupMaxLine = 512;  // including the "DCSETUP " prefix

// Feed it the bytes read from the serial port. Whatever it sends back goes to
// `write`, one complete line (without the leading line break the caller adds)
// per call. `write` may be called from another task, for the answer to `wifi`,
// so it must be safe to call concurrently with feed().
class SetupProtocol {
public:
    SetupProtocol(SetupInfo info, SetupBackend backend, std::function<void(const std::string&)> write)
        : info_(std::move(info)), backend_(std::move(backend)), write_(std::move(write)) {}

    void feed(const char* data, size_t length);

private:
    void handle_line(const std::string& line);
    void reply_error(const std::string& id_json, const char* code, const std::string& message,
                     const std::string& extra = "");
    void send(const std::string& json);
    void hello(const std::string& id);
    void wifi(const std::string& id, const std::string& ssid, const std::string& password);
    void forget(const std::string& id);
    void parts(const std::string& id);
    void declare(const std::string& id, const json::Value& request);

    SetupInfo info_;
    SetupBackend backend_;
    std::function<void(const std::string&)> write_;
    std::string line_;
    bool discarding_ = false;
    // Set while a `wifi` command is running; cleared by its `done`. Shared with
    // the task that calls `done`.
    std::shared_ptr<std::atomic<bool>> busy_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace dc_device::esp32
