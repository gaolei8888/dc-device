#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace dc_device::esp32 {

// Access token (docs/superpowers/specs/2026-10-20-access-token-design.md): one token per
// device, handed out only over the USB cable. The device keeps a SHA-256 hash of it, never
// the token. Portable code; storage, random numbers and the clock are injected so all of
// it runs in host tests.

enum class AuthResult { Allowed, Unauthorized, Unpaired, LockedOut };

// Error is not NotFound: a store that cannot be read must not make a paired device look
// unpaired (and so open). The device then fails closed until `token` is run again.
enum class LoadResult { Found, NotFound, Error };

struct AccessStorage {
    std::function<LoadResult(std::array<uint8_t, 32>&)> load;
    std::function<bool(const std::array<uint8_t, 32>&)> save;
    std::function<bool()> erase;
};

// URL-safe base64 without padding: 32 bytes -> 43 characters.
std::string token_encode(const uint8_t* bytes, size_t length);

class AccessControl {
public:
    // enforce_when_unpaired: true for release builds (a device with no token refuses
    // protected calls), false for development builds (open until paired).
    // clock: monotonic seconds.
    AccessControl(bool enforce_when_unpaired, AccessStorage storage,
                  std::function<void(uint8_t*, size_t)> random, std::function<double()> clock);

    bool paired() const;
    bool required() const;  // enforce_when_unpaired || paired()

    // `authorization` is the raw Authorization header value ("" = none sent).
    AuthResult check(const std::string& authorization);

    // Makes a new token, stores its hash (the old token stops working) and returns it in
    // token_out. False if storage failed; the old token then still works.
    bool issue(std::string& token_out);
    // Forgets the token. False if storage failed.
    bool revoke();

    unsigned consecutive_failures() const;
    std::string diagnostics_json() const;  // never contains the token or its hash

    static constexpr unsigned kMaxFailures = 10;
    static constexpr double kLockoutSeconds = 30.0;

private:
    bool locked_out_locked() const;

    mutable std::mutex mutex_;
    bool enforce_;
    AccessStorage storage_;
    std::function<void(uint8_t*, size_t)> random_;
    std::function<double()> clock_;
    bool paired_ = false;
    std::array<uint8_t, 32> hash_{};
    unsigned failures_ = 0;
    double locked_until_ = 0;
    bool locked_ = false;
};

}  // namespace dc_device::esp32
