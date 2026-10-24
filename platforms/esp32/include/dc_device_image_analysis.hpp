#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "dc_device_selfcheck.hpp"

namespace dc_device::esp32 {

// Plain facts about one grayscale frame. Host-testable: no camera needed.
struct ImageStats {
    size_t pixels = 0;
    int min = 0;
    int max = 0;
    double mean = 0;
    double stddev = 0;
};

ImageStats analyze_gray(const uint8_t* data, size_t length);

// What a frame says about the camera. A frame that arrived proves the sensor, the
// clock and the data lines work together. What the frame shows is another matter:
//  - no pixels, or every pixel the same value: the data lines are stuck -> Failed;
//  - nearly uniform (stddev < 2): the pipeline works but the picture carries nothing,
//    which is also what a covered lens or a dark room gives -> Unverified;
//  - anything else -> Ok.
Verdict judge_frame(const ImageStats& stats, std::string& evidence);

}  // namespace dc_device::esp32
