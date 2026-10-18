#pragma once

#include <cstddef>
#include <cstdint>

namespace dc_device::esp32 {

// Analysis of a block of microphone samples read from an I2S input as 32-bit
// words (a typical MEMS microphone sends 24 bits left-justified, so the low 8 bits
// are zero). Platform independent so it can be tested with synthetic data.
struct MicStats {
    size_t samples = 0;
    bool constant = false;      // every sample identical: nothing is driving the line
    double rms = 0;             // of the 24-bit value, as a fraction of full scale (0..1)
    double low_byte_zero = 0;   // fraction of samples whose low 8 bits are zero
    // "Looks like a microphone": the line moves, and it carries clean 24-bit words
    // rather than random bits (which is what a floating data pin looks like).
    bool looks_like_mic = false;
};

MicStats analyze_mic(const int32_t* samples, size_t n);

// Power of the signal at `freq_hz` (Goertzel), as a fraction of full scale squared.
double tone_power(const int32_t* samples, size_t n, int sample_rate_hz, double freq_hz);

}  // namespace dc_device::esp32
