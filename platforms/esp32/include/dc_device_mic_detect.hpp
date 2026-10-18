#pragma once

#ifdef ESP_PLATFORM

#include <vector>

#include "dc_device_selfcheck.hpp"

namespace dc_device::esp32 {

// Finds an I2S microphone with no pin table (docs/self-check-v0.1.md, class B).
//
// For each ordered pair of floating pins (the pin survey says which), the I2S
// peripheral drives a bit clock and a word clock on them while the other floating
// pins are sampled as inputs with a pull-down. A pin whose level moves with the
// clock is a data candidate. The three pins are then read as an I2S input and the
// samples are checked (dc_device_audio_analysis.hpp): a microphone sends clean
// 24-bit words, a floating pin sends random bits.
//
// This drives the two clock pins with push-pull outputs at the weakest drive
// strength, on pins nothing is known to be connected to. It runs only in Full mode
// (an explicit /self_test), within a time budget per run, and carries on from where
// it stopped. Quick returns what the last Full run found.
Detector make_mic_detector();

// For debugging a detector against a known wiring: reads an I2S input on the given
// pins and reports what the samples look like, and which of `others` move with the
// clock. Drives sck and ws.
struct MicTrial {
    bool read_ok = false;
    size_t samples = 0;
    bool constant = false;
    double rms = 0;
    double low_byte_zero = 0;
    bool looks_like_mic = false;
    std::vector<int> moving_pins;  // among `others`, with sck/ws driven
};
MicTrial try_mic(int sck, int ws, int sd, const std::vector<int>& others);

}  // namespace dc_device::esp32

#endif
