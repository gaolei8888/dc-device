#pragma once

#ifdef ESP_PLATFORM

#include <vector>

#include "dc_device_selfcheck.hpp"

namespace dc_device::esp32 {

struct PinPair {
    int sda;
    int scl;
};

// Detector for an OV2640 or OV5640 camera on a two-wire (SCCB) bus. It reads
// the sensor's chip id, so "found" means the sensor answered, not that a pin
// table said so.
//
// Quick: probes the cached wiring, else the `known_maps`. Full: also tries the
// chip's safe pin pairs (dc_device_pins.hpp). Only the two bus pins are used,
// open drain with pull-ups; no data line is driven and no frame is taken.
Detector make_camera_detector(std::vector<PinPair> known_maps);

}  // namespace dc_device::esp32

#endif
