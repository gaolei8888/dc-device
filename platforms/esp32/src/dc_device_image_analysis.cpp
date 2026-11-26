#include "dc_device_image_analysis.hpp"

#include <cmath>
#include <cstdio>

namespace dc_device::esp32 {

ImageStats analyze_gray(const uint8_t* data, size_t length) {
    ImageStats s;
    if (data == nullptr || length == 0) return s;
    s.pixels = length;
    s.min = 255;
    s.max = 0;
    double sum = 0, sum_sq = 0;
    for (size_t i = 0; i < length; ++i) {
        int v = data[i];
        if (v < s.min) s.min = v;
        if (v > s.max) s.max = v;
        sum += v;
        sum_sq += static_cast<double>(v) * v;
    }
    s.mean = sum / static_cast<double>(length);
    double var = sum_sq / static_cast<double>(length) - s.mean * s.mean;
    s.stddev = var > 0 ? std::sqrt(var) : 0;
    return s;
}

Verdict judge_frame(const ImageStats& s, std::string& evidence) {
    char buf[160];
    if (s.pixels == 0) {
        evidence = "the frame has no pixels";
        return Verdict::Failed;
    }
    if (s.min == s.max) {
        std::snprintf(buf, sizeof buf, "all %zu pixels have the value %d: the data lines look stuck", s.pixels, s.min);
        evidence = buf;
        return Verdict::Failed;
    }
    if (s.stddev < 2.0) {
        std::snprintf(buf, sizeof buf,
                      "frames arrive but the picture is almost uniform (values %d-%d): lens covered, or dark",
                      s.min, s.max);
        evidence = buf;
        return Verdict::Unverified;
    }
    std::snprintf(buf, sizeof buf, "a %zu-pixel frame with detail (values %d-%d, spread %.1f)", s.pixels, s.min,
                  s.max, s.stddev);
    evidence = buf;
    return Verdict::Ok;
}

}  // namespace dc_device::esp32
