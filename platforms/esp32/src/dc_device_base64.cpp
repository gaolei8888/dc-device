#include "dc_device_base64.hpp"

namespace dc_device {
namespace {
const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int value_of(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

std::string base64_encode(const uint8_t* data, size_t length) {
    std::string out;
    out.reserve((length + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= length; i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (length - i == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (length - i == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool base64_decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.size() % 4 != 0) return false;
    std::vector<uint8_t> result;
    result.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        bool last = i + 4 == text.size();
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            unsigned char c = static_cast<unsigned char>(text[i + k]);
            if (c == '=') {
                // Padding only in the last group, only at its end: "xx==" or "xxx=".
                if (!last || k < 2) return false;
                ++pad;
                v[k] = 0;
            } else {
                if (pad > 0) return false;  // a letter after '='
                v[k] = value_of(c);
                if (v[k] < 0) return false;
            }
        }
        uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        result.push_back(uint8_t(n >> 16));
        if (pad < 2) result.push_back(uint8_t(n >> 8));
        if (pad < 1) result.push_back(uint8_t(n));
    }
    out = std::move(result);
    return true;
}

}  // namespace dc_device
