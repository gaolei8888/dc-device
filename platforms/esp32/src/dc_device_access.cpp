#include "dc_device_access.hpp"

#include "dc_device_sha256.hpp"

namespace dc_device::esp32 {

std::string token_encode(const uint8_t* bytes, size_t length) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 3 <= length; i += 3) {
        uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (length - i == 1) {
        uint32_t v = uint32_t(bytes[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
    } else if (length - i == 2) {
        uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
    }
    return out;
}

namespace {

// Compares without stopping at the first difference.
bool same_hash(const std::array<uint8_t, 32>& a, const std::array<uint8_t, 32>& b) {
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= uint8_t(a[i] ^ b[i]);
    return diff == 0;
}

const char kScheme[] = "Bearer ";

}  // namespace

AccessControl::AccessControl(bool enforce_when_unpaired, AccessStorage storage,
                             std::function<void(uint8_t*, size_t)> random,
                             std::function<double()> clock)
    : enforce_(enforce_when_unpaired),
      storage_(std::move(storage)),
      random_(std::move(random)),
      clock_(std::move(clock)) {
    if (!storage_.load) return;
    LoadResult loaded = storage_.load(hash_);
    if (loaded == LoadResult::Found) {
        paired_ = true;
    } else if (loaded == LoadResult::Error) {
        // Paired, with a hash nothing can match (no input hashes to all zeros).
        hash_.fill(0);
        paired_ = true;
    }
}

bool AccessControl::paired() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paired_;
}

bool AccessControl::required() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enforce_ || paired_;
}

bool AccessControl::locked_out_locked() const { return locked_ && clock_() < locked_until_; }

AuthResult AccessControl::check(const std::string& authorization) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!paired_) return enforce_ ? AuthResult::Unpaired : AuthResult::Allowed;
    if (locked_) {
        if (clock_() < locked_until_) return AuthResult::LockedOut;
        locked_ = false;
        failures_ = 0;
    }
    if (authorization.empty()) return AuthResult::Unauthorized;  // no guess was made

    // Always hash something, so a wrong scheme and a wrong token take the same path.
    const size_t scheme_len = sizeof(kScheme) - 1;
    bool scheme_ok = authorization.compare(0, scheme_len, kScheme) == 0;
    std::string candidate = scheme_ok ? authorization.substr(scheme_len) : std::string();
    std::array<uint8_t, 32> hash = sha256(candidate.data(), candidate.size());
    bool match = same_hash(hash, hash_) && scheme_ok;
    if (match) {
        failures_ = 0;
        return AuthResult::Allowed;
    }
    if (++failures_ >= kMaxFailures) {
        locked_ = true;
        locked_until_ = clock_() + kLockoutSeconds;
    }
    return AuthResult::Unauthorized;
}

bool AccessControl::issue(std::string& token_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t bytes[32];
    random_(bytes, sizeof(bytes));
    // The hash is of the text the host will send, so check() hashes what it receives.
    std::string token = token_encode(bytes, sizeof(bytes));
    std::array<uint8_t, 32> hash = sha256(token.data(), token.size());
    if (storage_.save && !storage_.save(hash)) return false;
    hash_ = hash;
    paired_ = true;
    failures_ = 0;
    locked_ = false;
    token_out = token;
    return true;
}

bool AccessControl::revoke() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paired_ && storage_.erase && !storage_.erase()) return false;
    paired_ = false;
    hash_.fill(0);
    failures_ = 0;
    locked_ = false;
    return true;
}

unsigned AccessControl::consecutive_failures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failures_;
}

std::string AccessControl::diagnostics_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::string("{\"paired\":") + (paired_ ? "true" : "false") +
           ",\"consecutive_failures\":" + std::to_string(failures_) +
           ",\"locked_out\":" + (locked_out_locked() ? "true" : "false") + "}";
}

}  // namespace dc_device::esp32
