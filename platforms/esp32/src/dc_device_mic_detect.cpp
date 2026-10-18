#ifdef ESP_PLATFORM

#include "dc_device_mic_detect.hpp"

#include <cstdio>
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

constexpr int kRate = 16000;
constexpr int64_t kBudgetUs = 8 * 1000 * 1000;
constexpr int kCaptureSamples = 3000;
constexpr int kMinTransitions = 20;
constexpr size_t kReadSamples = 4800;

struct MicState {
    int sck = -1, ws = -1, sd = -1;
    MicStats stats;
    size_t cursor = 0;
    Finding last;
};

i2s_std_config_t std_config(int bclk, int ws, int dout, int din) {
    i2s_std_config_t c{};
    c.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
    c.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
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

// Reads `n` 32-bit words from an I2S input on (bclk, ws, din). Empty on failure.
std::vector<int32_t> read_words(int bclk, int ws, int din, size_t n) {
    std::vector<int32_t> words;
    i2s_chan_handle_t rx = nullptr;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, nullptr, &rx) != ESP_OK) return words;
    i2s_std_config_t sc = std_config(bclk, ws, -1, din);
    if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
        i2s_del_channel(rx);
        release(bclk, ws, din);
        return words;
    }
    gpio_set_drive_capability(static_cast<gpio_num_t>(bclk), GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(static_cast<gpio_num_t>(ws), GPIO_DRIVE_CAP_0);
    std::vector<int32_t> raw(n * 2);  // stereo: left, right
    size_t got = 0;
    i2s_channel_read(rx, raw.data(), raw.size() * sizeof(int32_t), &got, 500);  // discard: settling
    got = 0;
    if (i2s_channel_read(rx, raw.data(), raw.size() * sizeof(int32_t), &got, 1000) == ESP_OK) {
        size_t frames = got / (2 * sizeof(int32_t));
        // A mic answers on one slot. Keep the slot that moves.
        std::vector<int32_t> left, right;
        for (size_t i = 0; i < frames; ++i) {
            left.push_back(raw[2 * i]);
            right.push_back(raw[2 * i + 1]);
        }
        bool left_moves = false, right_moves = false;
        for (size_t i = 1; i < frames; ++i) {
            left_moves = left_moves || left[i] != left[0];
            right_moves = right_moves || right[i] != right[0];
        }
        words = (right_moves && !left_moves) ? right : left;
    }
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
    release(bclk, ws, din);
    return words;
}

// Drives bclk/ws and samples `inputs` as plain inputs with a pull-down. Returns the
// pins whose level changed at least kMinTransitions times.
std::vector<int> data_candidates(int bclk, int ws, const std::vector<int>& inputs) {
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
        esp_rom_delay_us(1500);  // let the clocks start
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

std::string describe(const MicStats& m) {
    char b[96];
    snprintf(b, sizeof(b), "clean 24-bit samples, level %.4f of full scale, %.0f%% clean words", m.rms,
             m.low_byte_zero * 100.0);
    return b;
}

}  // namespace

MicTrial try_mic(int sck, int ws, int sd, const std::vector<int>& others) {
    MicTrial r;
    esp_log_level_set("gpio", ESP_LOG_WARN);
    r.moving_pins = data_candidates(sck, ws, others);
    std::vector<int32_t> w = read_words(sck, ws, sd, kReadSamples);
    r.read_ok = !w.empty();
    MicStats m = analyze_mic(w.data(), w.size());
    r.samples = m.samples;
    r.constant = m.constant;
    r.rms = m.rms;
    r.low_byte_zero = m.low_byte_zero;
    r.looks_like_mic = m.looks_like_mic;
    return r;
}

Detector make_mic_detector() {
    auto st = std::make_shared<MicState>();
    st->last.name = "microphone";
    st->last.klass = PeripheralClass::Signal;
    st->last.verdict = Verdict::Unverified;
    st->last.evidence = "not searched yet; run the self test";

    Detector d;
    d.name = "microphone";
    d.klass = PeripheralClass::Signal;
    d.run_many = [st](Mode mode) {
        MicState& s = *st;
        if (mode != Mode::Full) return std::vector<Finding>{s.last};

        // Already found: read it again.
        if (s.sck >= 0) {
            std::vector<int32_t> w = read_words(s.sck, s.ws, s.sd, kReadSamples);
            MicStats m = analyze_mic(w.data(), w.size());
            Finding f = s.last;
            if (m.looks_like_mic) {
                s.stats = m;
                f.verdict = Verdict::Ok;
                f.evidence = describe(m);
            } else {
                f.verdict = Verdict::Failed;
                f.evidence = "the microphone found earlier on " + wiring(s) + " no longer sends samples";
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
            for (int sd : data_candidates(p.first, p.second, others)) {
                std::vector<int32_t> w = read_words(p.first, p.second, sd, kReadSamples);
                MicStats m = analyze_mic(w.data(), w.size());
                if (!m.looks_like_mic) continue;
                s.sck = p.first;
                s.ws = p.second;
                s.sd = sd;
                s.stats = m;
                Finding f;
                f.name = "microphone";
                f.klass = PeripheralClass::Signal;
                f.verdict = Verdict::Ok;
                f.evidence = describe(m);
                f.wiring = wiring(s);
                f.source = "scan";
                s.last = f;
                return std::vector<Finding>{f};
            }
        }
        if (s.cursor >= pairs.size()) s.cursor = 0;
        Finding f;
        f.name = "microphone";
        f.klass = PeripheralClass::Signal;
        if (cut) {
            f.verdict = Verdict::Unverified;
            f.evidence = "search not finished; run the self test again to continue";
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
