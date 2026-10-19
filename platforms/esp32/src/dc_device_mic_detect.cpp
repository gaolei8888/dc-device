#ifdef ESP_PLATFORM

#include "dc_device_mic_detect.hpp"

#include <cmath>
#include <cstdio>
#include <atomic>
#include <memory>
#include <vector>

#include "dc_device_audio_analysis.hpp"
#include "dc_device_pin_survey.hpp"
#include "dc_device_pins.hpp"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

namespace dc_device::esp32 {

namespace {

constexpr int64_t kBudgetUs = 8 * 1000 * 1000;
constexpr int kCaptureSamples = 3000;
// A quiet microphone holds most of its data bits still, so few transitions are
// enough to make a pin a candidate; the level and tone checks do the judging.
constexpr int kMinTransitions = 4;
constexpr double kToneGainMin = 8.0;
// A microphone sends nothing for a while after its first clock. Measured on a real
// board: 0 edges after 1.5 ms, 80 to 130 edges after 20 ms or more.
constexpr int kSettleMs = 25;
constexpr size_t kReadSamples = 4800;
// The verification reads half as much: the chip has no PSRAM here and about 250 KB of
// heap, and a read plus the tone's own buffer once ran it out (abort in `new`).
constexpr size_t kVerifySamples = 2400;

struct MicState {
    int sck = -1, ws = -1, sd = -1;
    MicStats stats;
    Finding fallback;           // a signal that did not answer the tone, kept in case nothing better turns up
    bool has_fallback = false;
    size_t cursor = 0;
    Finding last;
};

i2s_std_config_t std_config(int bclk, int ws, int dout, int din, const MicFormat& fmt = {}) {
    i2s_std_config_t c{};
    c.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(fmt.rate_hz));
    i2s_data_bit_width_t width = fmt.bits == 16 ? I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_32BIT;
    if (fmt.msb) {
        i2s_std_slot_config_t slot = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO);
        c.slot_cfg = slot;
    } else {
        i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO);
        c.slot_cfg = slot;
    }
    c.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    c.gpio_cfg.bclk = static_cast<gpio_num_t>(bclk);
    c.gpio_cfg.ws = static_cast<gpio_num_t>(ws);
    c.gpio_cfg.dout = dout < 0 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(dout);
    c.gpio_cfg.din = din < 0 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(din);
    return c;
}

void release(int a, int b, int c) {
    for (int p : {a, b, c}) {
        if (p >= 0) gpio_reset_pin(static_cast<gpio_num_t>(p));
    }
}

struct Slots {
    bool ok = false;
    std::vector<int32_t> left, right;  // left-aligned in 32 bits whatever the width read
};

// Reads `n` frames from an I2S input on (bclk, ws, din) in the given format.
Slots read_slots(int bclk, int ws, int din, size_t n, const MicFormat& fmt) {
    Slots out;
    i2s_chan_handle_t rx = nullptr;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, nullptr, &rx) != ESP_OK) return out;
    i2s_std_config_t sc = std_config(bclk, ws, -1, din, fmt);
    if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
        i2s_del_channel(rx);
        release(bclk, ws, din);
        return out;
    }
    gpio_set_drive_capability(static_cast<gpio_num_t>(bclk), GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(static_cast<gpio_num_t>(ws), GPIO_DRIVE_CAP_0);
    const bool wide = fmt.bits != 16;
    const size_t word = wide ? sizeof(int32_t) : sizeof(int16_t);
    std::vector<uint8_t> raw(n * 2 * word);  // stereo: left, right
    size_t got = 0;
    i2s_channel_read(rx, raw.data(), raw.size(), &got, 500);  // discard: settling
    got = 0;
    if (i2s_channel_read(rx, raw.data(), raw.size(), &got, 1000) == ESP_OK) {
        out.ok = true;
        size_t frames = got / (2 * word);
        out.left.reserve(frames);
        out.right.reserve(frames);
        for (size_t i = 0; i < frames; ++i) {
            int32_t l, r;
            if (wide) {
                const int32_t* w = reinterpret_cast<const int32_t*>(raw.data());
                l = w[2 * i];
                r = w[2 * i + 1];
            } else {
                const int16_t* w = reinterpret_cast<const int16_t*>(raw.data());
                l = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(w[2 * i])) << 16);
                r = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(w[2 * i + 1])) << 16);
            }
            out.left.push_back(l);
            out.right.push_back(r);
        }
    }
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
    release(bclk, ws, din);
    return out;
}

bool moves(const std::vector<int32_t>& v) {
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i] != v[0]) return true;
    }
    return false;
}

// Level of the varying part of the signal (the mean removed), as a fraction of full scale.
double ac_rms(const std::vector<int32_t>& v) {
    if (v.empty()) return 0;
    double mean = 0;
    for (int32_t x : v) mean += x;
    mean /= static_cast<double>(v.size());
    double sum = 0;
    for (int32_t x : v) {
        double d = static_cast<double>(x) - mean;
        sum += d * d;
    }
    return std::sqrt(sum / static_cast<double>(v.size())) / 2147483648.0;
}

// The detector's read: 32-bit Philips. A mic answers on one slot; keep the one that moves.
std::vector<int32_t> read_words(int bclk, int ws, int din, size_t n) {
    Slots s = read_slots(bclk, ws, din, n, MicFormat{});
    if (!s.ok) return {};
    return (moves(s.right) && !moves(s.left)) ? s.right : s.left;
}

// Drives bclk/ws and samples `inputs` as plain inputs with a pull-down. Returns the
// pins whose level changed at least kMinTransitions times.
std::vector<int> data_candidates(int bclk, int ws, const std::vector<int>& inputs, int settle_ms = 0,
                                 std::vector<std::pair<int, int>>* counts = nullptr) {
    std::vector<int> found;
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) return found;
    i2s_std_config_t sc = std_config(bclk, ws, -1, -1);
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) {
        i2s_del_channel(tx);
        return found;
    }
    for (int p : inputs) {
        gpio_num_t g = static_cast<gpio_num_t>(p);
        gpio_set_direction(g, GPIO_MODE_INPUT);
        gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY);
    }
    gpio_set_drive_capability(static_cast<gpio_num_t>(bclk), GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(static_cast<gpio_num_t>(ws), GPIO_DRIVE_CAP_0);
    if (i2s_channel_enable(tx) == ESP_OK) {
        // Let the clocks start. A microphone may send nothing for tens of milliseconds
        // after the first clock; `settle_ms` is for finding out how long.
        if (settle_ms >= 10) vTaskDelay(pdMS_TO_TICKS(settle_ms));
        else esp_rom_delay_us(settle_ms > 0 ? settle_ms * 1000 : 1500);
        std::vector<uint32_t> lo(kCaptureSamples), hi(kCaptureSamples);
        for (int i = 0; i < kCaptureSamples; ++i) {
            lo[i] = REG_READ(GPIO_IN_REG);
            hi[i] = REG_READ(GPIO_IN1_REG);
        }
        i2s_channel_disable(tx);
        for (int p : inputs) {
            int transitions = 0;
            bool prev = false;
            for (int i = 0; i < kCaptureSamples; ++i) {
                bool v = p < 32 ? ((lo[i] >> p) & 1) : ((hi[i] >> (p - 32)) & 1);
                if (i > 0 && v != prev) ++transitions;
                prev = v;
            }
            if (counts && transitions > 0) counts->push_back({p, transitions});
            if (transitions >= kMinTransitions) found.push_back(p);
        }
    }
    i2s_del_channel(tx);
    release(bclk, ws, -1);
    for (int p : inputs) gpio_reset_pin(static_cast<gpio_num_t>(p));
    return found;
}

std::string wiring(const MicState& s) {
    return "SCK " + std::to_string(s.sck) + ", WS " + std::to_string(s.ws) + ", SD " + std::to_string(s.sd);
}

struct Verification {
    bool plausible = false;
    bool tone_tested = false;
    double level = 0;
    double gain = 0;
    bool tone_ok() const { return tone_tested && gain >= kToneGainMin; }
};

struct ToneJob {
    MicStimulus stimulus;
    std::shared_ptr<std::atomic<bool>> done;
};

void tone_task(void* arg) {
    std::unique_ptr<ToneJob> job(static_cast<ToneJob*>(arg));
    job->stimulus.play(job->stimulus.tone_hz, job->stimulus.tone_ms);
    job->done->store(true);
    vTaskDelete(nullptr);
}

// Reads the candidate once quietly; if the level is plausible and a stimulus exists,
// reads it again while the speaker plays and compares the energy at the tone.
Verification verify(int sck, int ws, int sd, const MicStimulus& stim) {
    Verification v;
    std::vector<int32_t> quiet = read_words(sck, ws, sd, kVerifySamples);
    SignalStats qs = signal_stats(quiet.data(), quiet.size());
    v.level = qs.ac_rms;
    v.plausible = plausible_mic_level(qs);
    if (!v.plausible || !stim.play) return v;

    auto done = std::make_shared<std::atomic<bool>>(false);
    auto* job = new ToneJob{stim, done};
    if (xTaskCreate(tone_task, "dc_tone", 6144, job, 5, nullptr) != pdPASS) {
        delete job;
        return v;
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // let the tone start
    std::vector<int32_t> loud = read_words(sck, ws, sd, kVerifySamples);
    for (int i = 0; i < 250 && !done->load(); ++i) vTaskDelay(pdMS_TO_TICKS(20));
    if (!done->load() || loud.empty()) return v;  // the tone did not finish: no verdict from it
    v.tone_tested = true;
    v.gain = tone_gain(quiet.data(), quiet.size(), loud.data(), loud.size(), 16000, stim.tone_hz);
    return v;
}

Finding make_finding(const MicState& s, const Verification& v, const MicStimulus& stim) {
    Finding f;
    f.name = "microphone";
    f.klass = PeripheralClass::Signal;
    f.wiring = wiring(s);
    f.source = "scan";
    char b[200];
    if (v.tone_ok()) {
        f.verdict = Verdict::Ok;
        // The gain divides by a very small quiet level, so a large number says nothing
        // beyond "far above the threshold"; do not print it as a measurement.
        snprintf(b, sizeof(b), "answers a %d Hz tone from the speaker (more than %.0f times stronger at that frequency), level %.4f of full scale",
                 stim.tone_hz, kToneGainMin, v.level);
    } else if (v.tone_tested) {
        f.verdict = Verdict::Unverified;
        snprintf(b, sizeof(b), "a signal at level %.4f of full scale, but it did not answer a %d Hz tone from the speaker (%.1f times, needs %.0f)",
                 v.level, stim.tone_hz, v.gain, kToneGainMin);
    } else {
        f.verdict = Verdict::Unverified;
        snprintf(b, sizeof(b), "a signal at level %.4f of full scale that could not be tested with a sound; a person must speak and check",
                 v.level);
    }
    f.evidence = b;
    return f;
}

}  // namespace

MicTrial try_mic(int sck, int ws, int sd, const std::vector<int>& others, const MicFormat& fmt) {
    MicTrial r;
    esp_log_level_set("gpio", ESP_LOG_WARN);
    r.moving_pins = data_candidates(sck, ws, others, fmt.settle_ms, &r.transitions);
    Slots sl = read_slots(sck, ws, sd, kReadSamples, fmt);
    r.read_ok = sl.ok && !sl.left.empty();
    r.left_ac_rms = ac_rms(sl.left);
    r.right_ac_rms = ac_rms(sl.right);
    const std::vector<int32_t>* use = &sl.left;
    if (fmt.slot == 2 || (fmt.slot == 0 && moves(sl.right) && !moves(sl.left))) use = &sl.right;
    for (size_t i = 0; i < use->size() && i < 8; ++i) r.preview.push_back((*use)[i]);
    MicStats m = analyze_mic(use->data(), use->size());
    r.samples = m.samples;
    r.constant = m.constant;
    r.rms = m.rms;
    r.low_byte_zero = m.low_byte_zero;
    r.looks_like_mic = m.looks_like_mic;
    return r;
}

Detector make_mic_detector(MicStimulus stimulus) {
    auto st = std::make_shared<MicState>();
    st->last.name = "microphone";
    st->last.klass = PeripheralClass::Signal;
    st->last.verdict = Verdict::Unverified;
    st->last.evidence = "not searched yet; run the self test";

    Detector d;
    d.name = "microphone";
    d.klass = PeripheralClass::Signal;
    d.run_many = [st, stimulus](Mode mode) {
        MicState& s = *st;
        if (mode != Mode::Full) return std::vector<Finding>{s.last};

        // Already found: check it again.
        if (s.sck >= 0) {
            Verification v = verify(s.sck, s.ws, s.sd, stimulus);
            Finding f;
            if (v.plausible) {
                f = make_finding(s, v, stimulus);
            } else {
                f = s.last;
                f.verdict = Verdict::Failed;
                f.evidence = "the microphone found earlier on " + wiring(s) + " no longer sends a usable signal";
            }
            s.last = f;
            return std::vector<Finding>{f};
        }

        // The gpio driver logs one line per pin it configures; a search configures
        // thousands, and the serial port at 115200 baud turned that into most of
        // the run time.
        esp_log_level_set("gpio", ESP_LOG_WARN);
        std::vector<int> floating = pins_floating();
        std::vector<std::pair<int, int>> pairs = ordered_pairs(floating);
        int64_t start = esp_timer_get_time();
        if (s.cursor >= pairs.size()) s.cursor = 0;
        bool cut = false;
        while (s.cursor < pairs.size()) {
            if (esp_timer_get_time() - start > kBudgetUs) {
                cut = true;
                break;
            }
            auto p = pairs[s.cursor++];
            vTaskDelay(1);  // let the idle task feed the watchdog
            std::vector<int> others;
            for (int q : floating) {
                if (q != p.first && q != p.second) others.push_back(q);
            }
            for (int sd : data_candidates(p.first, p.second, others, kSettleMs)) {
                Verification v = verify(p.first, p.second, sd, stimulus);
                if (!v.plausible) continue;
                MicState found = s;
                found.sck = p.first;
                found.ws = p.second;
                found.sd = sd;
                Finding f = make_finding(found, v, stimulus);
                if (v.tone_ok()) {
                    s.sck = found.sck;
                    s.ws = found.ws;
                    s.sd = found.sd;
                    s.last = f;
                    return std::vector<Finding>{f};
                }
                // A signal that did not answer: keep it as the best guess and carry on.
                s.fallback = f;
                s.has_fallback = true;
            }
        }
        if (s.cursor >= pairs.size()) s.cursor = 0;
        Finding f;
        f.name = "microphone";
        f.klass = PeripheralClass::Signal;
        if (cut) {
            f.verdict = Verdict::Unverified;
            f.evidence = "search not finished; run the self test again to continue";
        } else if (s.has_fallback) {
            f = s.fallback;
        } else {
            f.verdict = Verdict::NotFound;
            f.evidence = "no microphone found on " + std::to_string(pairs.size()) + " clock pairs";
        }
        s.last = f;
        return std::vector<Finding>{f};
    };
    return d;
}

}  // namespace dc_device::esp32

#endif
