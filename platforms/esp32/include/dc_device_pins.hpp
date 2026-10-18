#pragma once

#include <string>
#include <utility>
#include <vector>

namespace dc_device::esp32 {

// Pins a wiring search may try as a two-wire bus, per chip
// (docs/self-check-v0.1.md, "How the wiring is found"). One table, kept per chip,
// not guessed per board. Returns an empty list for a chip it does not know:
// no table means no search.
//
// ESP32-S3: left out are the flash and PSRAM pins (26-32, and 33-37 because the
// PSRAM may be octal), USB (19, 20), the console UART (43, 44) and the strapping
// pins (0, 3, 45, 46).
std::vector<int> safe_scan_pins(const std::string& chip_model);

// Every ordered (first, second) pair of distinct pins from `pins`.
std::vector<std::pair<int, int>> ordered_pairs(const std::vector<int>& pins);

// Which pins a detector is holding open (an I2C bus keeps its two pins configured).
// A detector that resets or reconfigures pins must leave the claimed ones alone.
void claim_pin(int pin);
void release_pin(int pin);
bool pin_claimed(int pin);

}  // namespace dc_device::esp32
