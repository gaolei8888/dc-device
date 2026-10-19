#pragma once

#ifdef ESP_PLATFORM

#include <functional>
#include <utility>
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
//
// A line that moves is not a microphone. A candidate is called working only if it
// answers a known sound, so the board needs a way to make one: `stimulus.play`
// should play a tone of `hz` for `ms` on the board's speaker and return when done.
// It runs on another task while the candidate is read. Without it a candidate that
// has a plausible level is reported `unverified`, never `ok`. The stimulus is played
// only when a candidate has a plausible level, in Full mode, so a board with no
// microphone stays silent. A self test on a board that has one is therefore audible.
struct MicStimulus {
    std::function<void(int hz, int ms)> play;
    int tone_hz = 880;
    int tone_ms = 1100;
};
Detector make_mic_detector(MicStimulus stimulus = {});

// For debugging a detector against a known wiring: reads an I2S input on the given
// pins and reports what the samples look like, and which of `others` move with the
// clock. Drives sck and ws.
// How the I2S input is read. The detector uses the defaults (Philips, 32 bits, 16 kHz,
// whichever slot moves). A host that tries other formats sets them here.
struct MicFormat {
    bool msb = false;    // left-justified (MSB) instead of Philips (data one clock after WS)
    int bits = 32;       // 16 or 32 bits per slot
    int slot = 0;        // 0: the slot that moves, 1: left, 2: right
    int rate_hz = 16000;
    int settle_ms = 0;   // wait after the clocks start before looking for the data pin; 0 = 1.5 ms
};

struct MicTrial {
    bool read_ok = false;
    size_t samples = 0;
    bool constant = false;
    double rms = 0;
    double low_byte_zero = 0;
    bool looks_like_mic = false;
    std::vector<int> moving_pins;  // among `others`, with sck/ws driven
    std::vector<std::pair<int, int>> transitions;  // (pin, edges seen) for every pin that moved at all
    // Level of the varying part of each slot, as a fraction of full scale. A slot
    // that carries sound moves with it; a floating or unused slot is flat or random.
    double left_ac_rms = 0;
    double right_ac_rms = 0;
    std::vector<int32_t> preview;  // the first samples of the chosen slot, left-aligned in 32 bits
};
MicTrial try_mic(int sck, int ws, int sd, const std::vector<int>& others, const MicFormat& fmt = {});

}  // namespace dc_device::esp32

#endif
