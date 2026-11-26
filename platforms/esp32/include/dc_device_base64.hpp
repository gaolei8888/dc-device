#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dc_device::esp32 {

// Strict standard-alphabet Base64 decode (RFC 4648, padding required). Returns false
// for anything else: a wrong length, a stray character, padding in the middle. The
// caller gets nothing half-decoded. Host-testable: no ESP-IDF needed.
inline bool base64_decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.size() % 4 != 0) return false;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            char c = text[i + k];
            if (c == '=') {
                // Padding is only allowed in the last two places of the last group.
                if (!last || k < 2) return (out.clear(), false);
                v[k] = 0;
                ++pad;
            } else {
                if (pad > 0) return (out.clear(), false);  // data after padding
                v[k] = value(c);
                if (v[k] < 0) return (out.clear(), false);
            }
        }
        const uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        out.push_back(uint8_t(n >> 16));
        if (pad < 2) out.push_back(uint8_t(n >> 8));
        if (pad < 1) out.push_back(uint8_t(n));
    }
    return true;
}

}  // namespace dc_device::esp32
