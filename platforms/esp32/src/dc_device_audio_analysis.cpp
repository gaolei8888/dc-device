#include "dc_device_audio_analysis.hpp"

#include <cmath>

namespace dc_device::esp32 {

MicStats analyze_mic(const int32_t* samples, size_t n) {
    MicStats s;
    s.samples = n;
    if (n == 0) return s;
    bool same = true;
    size_t low_zero = 0;
    double sum_sq = 0;
    for (size_t i = 0; i < n; ++i) {
        if (samples[i] != samples[0]) same = false;
        if ((samples[i] & 0xFF) == 0) ++low_zero;
        double v = static_cast<double>(samples[i] >> 8) / 8388608.0;  // 24-bit, -1..1
        sum_sq += v * v;
    }
    s.constant = same;
    s.low_byte_zero = static_cast<double>(low_zero) / static_cast<double>(n);
    s.rms = std::sqrt(sum_sq / static_cast<double>(n));
    s.looks_like_mic = !same && s.low_byte_zero > 0.9;
    return s;
}

double tone_power(const int32_t* samples, size_t n, int sample_rate_hz, double freq_hz) {
    if (n == 0) return 0;
    const double kPi = 3.14159265358979323846;
    double w = 2.0 * kPi * freq_hz / static_cast<double>(sample_rate_hz);
    double coeff = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        double v = static_cast<double>(samples[i] >> 8) / 8388608.0;
        double s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return power / (static_cast<double>(n) * static_cast<double>(n));
}

}  // namespace dc_device::esp32
