#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dc_device_base64.hpp"
#include "dc_device_json.hpp"

namespace dc_device::esp32 {

// One rectangle of pixels for the screen: where it goes and its RGB565 bytes (high
// byte first, as the panel wants them). Checked here so a bad request is refused
// before anything is sent to the panel. Host-testable: no ESP-IDF needed.
struct ScreenRect {
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<uint8_t> pixels;  // w * h * 2 bytes
};

constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 320;

inline bool whole_number(const json::Value* v, int& out) {
    if (v == nullptr || !v->is_number()) return false;
    const double d = v->number;
    if (d != d || d < -100000 || d > 100000 || d != static_cast<double>(static_cast<int>(d))) return false;
    out = static_cast<int>(d);
    return true;
}

// Fills `out` and returns true, or returns false with a one-sentence reason in `error`.
inline bool parse_draw_rect(const json::Value& args, ScreenRect& out, std::string& error) {
    ScreenRect r;
    if (!whole_number(args.find("x"), r.x) || !whole_number(args.find("y"), r.y) ||
        !whole_number(args.find("w"), r.w) || !whole_number(args.find("h"), r.h)) {
        error = "x, y, w and h must be whole numbers";
        return false;
    }
    if (r.w < 1 || r.h < 1 || r.x < 0 || r.y < 0 || r.x + r.w > kScreenWidth || r.y + r.h > kScreenHeight) {
        error = "the rectangle must lie inside the 240x320 screen";
        return false;
    }
    const json::Value* data = args.find("data");
    if (data == nullptr || !data->is_string()) {
        error = "data must be a Base64 string of RGB565 pixels";
        return false;
    }
    if (!base64_decode(data->string, r.pixels)) {
        error = "data is not valid Base64";
        return false;
    }
    if (r.pixels.size() != static_cast<size_t>(r.w) * static_cast<size_t>(r.h) * 2) {
        error = "data must hold exactly w*h pixels, 2 bytes each";
        return false;
    }
    out = std::move(r);
    return true;
}

}  // namespace dc_device::esp32
