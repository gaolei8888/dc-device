#pragma once

#ifdef ESP_PLATFORM

#include <vector>

#include "dc_device_selfcheck.hpp"

namespace dc_device::esp32 {

// Blind, input-only survey of the chip's safe pins (dc_device_pins.hpp): each pin
// is read with a pull-up and then with a pull-down. A pin that follows the pull is
// connected to nothing that holds it; a pin that does not is held by something on
// the board (an output, or an external pull). Drives nothing, needs no pin table.
// It says where something is, not what. Runs only in Full mode.
Detector make_pin_survey_detector();

// Safe pins that stay high even with the pull-down on: something on the board
// holds them up (an external pull-up, as an I2C bus needs). Skips pins claimed by
// a bus. Input only; takes a few milliseconds.
std::vector<int> pins_held_high();

// Safe pins that follow the pull (nothing holds them) and are not claimed.
std::vector<int> pins_floating();

}  // namespace dc_device::esp32

#endif
