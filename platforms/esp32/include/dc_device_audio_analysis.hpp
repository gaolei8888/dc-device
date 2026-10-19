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

// What the varying part of a block of samples looks like, whatever the word format.
struct SignalStats {
    size_t samples = 0;
    bool constant = false;  // every sample identical
    double ac_rms = 0;      // of the signal with its mean removed, fraction of full scale (0..1)
};
SignalStats signal_stats(const int32_t* samples, size_t n);

// A level a microphone can have: the line moves, but nowhere near the full-scale
// noise of a floating pin (random bits give about 0.29) or a clock picked up by
// crosstalk. This is a necessary condition, not proof: only a response to a known
// sound is (tone_gain).
bool plausible_mic_level(const SignalStats& s);

// How much stronger the signal at `freq_hz` is in `loud` than in `quiet`. About 1
// when the tone made no difference.
double tone_gain(const int32_t* quiet, size_t nq, const int32_t* loud, size_t nl, int sample_rate_hz,
                 double freq_hz);

// Power of the signal at `freq_hz` (Goertzel), as a fraction of full scale squared.
double tone_power(const int32_t* samples, size_t n, int sample_rate_hz, double freq_hz);

}  // namespace dc_device::esp32
