#pragma once

#ifdef ESP_PLATFORM

#include <vector>

#include "dc_device_selfcheck.hpp"

namespace dc_device::esp32 {

struct PinPair {
    int sda;
    int scl;
};

// Finds the two-wire (I2C/SCCB) buses on the board and lists every device on them,
// identified by chip id where the table knows the chip (dc_device_i2c_ids.hpp):
// camera sensors, IMUs, audio codecs, touch controllers, environment sensors. A
// device that answers but is not recognised is still reported, with a guess
// marked as one.
//
// Quick: re-checks the buses already found and the `known_maps`; cheap. Full: also
// searches the chip's safe pin pairs (dc_device_pins.hpp) for further buses, up to
// two, within a time budget per run. A search cut short says so in a row named
// "i2c_scan" and carries on from where it stopped the next time.
//
// Only the two bus pins are used, open drain with pull-ups; no data line is driven.
Detector make_i2c_detector(std::vector<PinPair> known_maps);

}  // namespace dc_device::esp32

#endif
