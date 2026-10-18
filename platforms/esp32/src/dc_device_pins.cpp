#include "dc_device_pins.hpp"

#include <set>

namespace dc_device::esp32 {

namespace {
std::set<int>& claimed() {
    static std::set<int> pins;
    return pins;
}
}  // namespace

void claim_pin(int pin) { claimed().insert(pin); }
void release_pin(int pin) { claimed().erase(pin); }
bool pin_claimed(int pin) { return claimed().count(pin) != 0; }

std::vector<int> safe_scan_pins(const std::string& chip_model) {
    std::vector<int> pins;
    if (chip_model == "esp32s3") {
        for (int p : {1, 2}) pins.push_back(p);
        for (int p = 4; p <= 18; ++p) pins.push_back(p);
        pins.push_back(21);
        for (int p = 38; p <= 42; ++p) pins.push_back(p);
        pins.push_back(47);
        pins.push_back(48);
    }
    return pins;
}

std::vector<std::pair<int, int>> ordered_pairs(const std::vector<int>& pins) {
    std::vector<std::pair<int, int>> out;
    for (int a : pins) {
        for (int b : pins) {
            if (a != b) out.emplace_back(a, b);
        }
    }
    return out;
}

}  // namespace dc_device::esp32
