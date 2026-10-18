#ifdef ESP_PLATFORM

#include "dc_device_i2c_detect.hpp"

#include <cstdio>
#include <memory>

#include "dc_device_chip.hpp"
#include "dc_device_i2c_ids.hpp"
#include "dc_device_pin_survey.hpp"
#include "dc_device_pins.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

namespace dc_device::esp32 {

namespace {

constexpr int kProbeTimeoutMs = 20;
constexpr int kXferTimeoutMs = 50;
constexpr int64_t kScanBudgetUs = 8 * 1000 * 1000;  // per Full run
constexpr size_t kMaxBuses = 2;                     // the S3 has two I2C controllers

struct Bus {
    int sda = -1;
    int scl = -1;
    int port = 0;
    i2c_master_bus_handle_t handle = nullptr;
    std::string source;  // "known_map" or "scan"
    std::vector<uint8_t> addrs;
};

struct State {
    std::vector<Bus> buses;
    std::vector<PinPair> known_maps;
    size_t cursor = 0;  // next pin pair of the search
};

std::string wiring_text(const Bus& b) {
    return "SDA " + std::to_string(b.sda) + ", SCL " + std::to_string(b.scl);
}

bool in_use(const State& st, int pin) {
    for (const Bus& b : st.buses) {
        if (b.sda == pin || b.scl == pin) return true;
    }
    return false;
}

bool open_bus(Bus& b, int port, int sda, int scl) {
    // Pull both lines up and let them settle before the bus is built. On a real
    // board a bus on GPIO 47/48 was missed by the first scan after boot and found
    // from the second one on, after a pin survey had left pull-ups on.
    gpio_set_pull_mode(static_cast<gpio_num_t>(sda), GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(static_cast<gpio_num_t>(scl), GPIO_PULLUP_ONLY);
    esp_rom_delay_us(1000);
    i2c_master_bus_config_t cfg{};
    cfg.i2c_port = port;
    cfg.sda_io_num = static_cast<gpio_num_t>(sda);
    cfg.scl_io_num = static_cast<gpio_num_t>(scl);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    b.sda = sda;
    b.scl = scl;
    b.port = port;
    b.handle = nullptr;
    return i2c_new_master_bus(&cfg, &b.handle) == ESP_OK;
}

void close_bus(Bus& b) {
    release_pin(b.sda);
    release_pin(b.scl);
    if (b.handle) i2c_del_master_bus(b.handle);
    b.handle = nullptr;
    gpio_reset_pin(static_cast<gpio_num_t>(b.sda));
    gpio_reset_pin(static_cast<gpio_num_t>(b.scl));
}

// Register access on one device of a bus. A device handle is made for the call
// and dropped after it; the bus stays open.
I2cAccess access_for(i2c_master_bus_handle_t bus) {
    I2cAccess io;
    io.read = [bus](uint8_t addr, uint16_t reg, int reg_bytes, uint8_t* out) {
        i2c_device_config_t dc{};
        dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dc.device_address = addr;
        dc.scl_speed_hz = 100000;
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) return false;
        uint8_t a[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)};
        esp_err_t e = reg_bytes == 2 ? i2c_master_transmit_receive(dev, a, 2, out, 1, kXferTimeoutMs)
                                     : i2c_master_transmit_receive(dev, &a[1], 1, out, 1, kXferTimeoutMs);
        i2c_master_bus_rm_device(dev);
        return e == ESP_OK;
    };
    io.write = [bus](uint8_t addr, uint8_t reg, uint8_t value) {
        i2c_device_config_t dc{};
        dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dc.device_address = addr;
        dc.scl_speed_hz = 100000;
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) return false;
        uint8_t buf[2] = {reg, value};
        esp_err_t e = i2c_master_transmit(dev, buf, 2, kXferTimeoutMs);
        i2c_master_bus_rm_device(dev);
        return e == ESP_OK;
    };
    return io;
}

// The board's pull-ups are the chip's internal ones (about 45 kOhm), weaker than
// an I2C bus wants, and a single probe sometimes gets an acknowledge from nothing
// (seen at random addresses on a real board). So a device counts only when three
// probes in a row answer, and a known device is gone only when three in a row do not.
bool answers_three_times(i2c_master_bus_handle_t bus, uint8_t addr) {
    for (int i = 0; i < 3; ++i) {
        if (i2c_master_probe(bus, addr, kProbeTimeoutMs) != ESP_OK) return false;
    }
    return true;
}

bool answers_at_least_once(i2c_master_bus_handle_t bus, uint8_t addr) {
    for (int i = 0; i < 3; ++i) {
        if (i2c_master_probe(bus, addr, kProbeTimeoutMs) == ESP_OK) return true;
    }
    return false;
}

// True if anything on the bus answered at one of the table's addresses. A probe
// that times out ends the attempt at once (a stuck bus can hang the driver).
bool any_answers(i2c_master_bus_handle_t bus) {
    size_t n = 0;
    const uint8_t* addrs = i2c_discovery_addresses(&n);
    for (size_t i = 0; i < n; ++i) {
        esp_err_t e = i2c_master_probe(bus, addrs[i], kProbeTimeoutMs);
        if (e == ESP_ERR_TIMEOUT) return false;
        if (e == ESP_OK && answers_three_times(bus, addrs[i])) return true;
    }
    return false;
}

// Every address 0x08..0x77 that answers three times in a row.
void list_devices(Bus& b) {
    b.addrs.clear();
    for (uint8_t a = 0x08; a < 0x78; ++a) {
        esp_err_t e = i2c_master_probe(b.handle, a, kProbeTimeoutMs);
        if (e == ESP_ERR_TIMEOUT) break;
        if (e == ESP_OK && answers_three_times(b.handle, a)) b.addrs.push_back(a);
    }
}

int free_port(const State& st) {
    for (int p = 0; p < 2; ++p) {
        bool used = false;
        for (const Bus& b : st.buses) used = used || b.port == p;
        if (!used) return p;
    }
    return -1;
}

// Tries (sda, scl) as a bus. Keeps it open and returns true if something answers.
bool try_pair(State& st, int sda, int scl, const char* source) {
    int port = free_port(st);
    if (port < 0) return false;
    Bus b;
    if (!open_bus(b, port, sda, scl)) return false;
    if (!any_answers(b.handle)) {
        close_bus(b);
        return false;
    }
    b.source = source;
    list_devices(b);
    if (b.addrs.empty()) {  // never keep a bus with nothing on it
        close_bus(b);
        return false;
    }
    claim_pin(sda);
    claim_pin(scl);
    st.buses.push_back(std::move(b));
    return true;
}

void report_bus(const Bus& b, std::vector<Finding>& out) {
    I2cAccess io = access_for(b.handle);
    for (uint8_t a : b.addrs) {
        I2cIdentity id = identify_i2c(a, io);
        Finding f;
        f.verdict = Verdict::Ok;
        f.evidence = id.evidence;
        f.wiring = wiring_text(b);
        f.source = b.source;
        f.name = i2c_finding_name(id, a);
        f.klass = PeripheralClass::Bus;
        out.push_back(std::move(f));
    }
}

// Re-checks the devices of a bus found earlier. A bus whose devices all vanished
// is closed so the next run starts again from the known maps.
void recheck_bus(Bus& b, std::vector<Finding>& out) {
    I2cAccess io = access_for(b.handle);
    std::vector<uint8_t> still;
    for (uint8_t a : b.addrs) {
        esp_err_t e = answers_at_least_once(b.handle, a) ? ESP_OK : ESP_FAIL;
        I2cIdentity id = e == ESP_OK ? identify_i2c(a, io) : I2cIdentity{};
        Finding f;
        f.wiring = wiring_text(b);
        f.source = b.source;
        f.klass = PeripheralClass::Bus;
        if (e == ESP_OK) {
            f.verdict = Verdict::Ok;
            f.evidence = id.evidence;
            f.name = i2c_finding_name(id, a);
            still.push_back(a);
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%02x", a);
            f.verdict = Verdict::Failed;
            f.evidence = std::string("the device found earlier at ") + buf + " no longer answers";
            f.name = std::string("i2c@") + buf;
        }
        out.push_back(std::move(f));
    }
    b.addrs = still;
}

}  // namespace

Detector make_i2c_detector(std::vector<PinPair> known_maps) {
    auto state = std::make_shared<State>();
    state->known_maps = std::move(known_maps);

    Detector d;
    d.name = "i2c";
    d.klass = PeripheralClass::Bus;
    d.run_many = [state](Mode mode) {
        State& st = *state;
        std::vector<Finding> out;

        // 1. Buses already found.
        for (size_t i = 0; i < st.buses.size();) {
            recheck_bus(st.buses[i], out);
            if (st.buses[i].addrs.empty()) {
                close_bus(st.buses[i]);
                st.buses.erase(st.buses.begin() + i);
            } else {
                ++i;
            }
        }

        // 2. The known maps, when nothing is open on those pins yet.
        bool tried_known = false;
        for (const PinPair& m : st.known_maps) {
            if (in_use(st, m.sda) || in_use(st, m.scl)) continue;
            tried_known = true;
            if (try_pair(st, m.sda, m.scl, "known_map")) report_bus(st.buses.back(), out);
        }

        // 3a. Full only: a bus needs pull-ups, so look first at pairs among the pins
        // that something holds high. A handful of pairs, a few milliseconds.
        if (mode == Mode::Full && st.buses.size() < kMaxBuses) {
            for (const auto& p : ordered_pairs(pins_held_high())) {
                if (st.buses.size() >= kMaxBuses) break;
                if (in_use(st, p.first) || in_use(st, p.second)) continue;
                if (try_pair(st, p.first, p.second, "scan")) report_bus(st.buses.back(), out);
            }
        }

        // 3b. Full only, and only when no bus is known yet: search every safe pin
        // pair. With one bus already found this is skipped, so a second bus that has
        // no external pull-ups can be missed; the search costs several seconds.
        size_t pairs_total = 0, pairs_done = 0;
        bool cut_short = false;
        if (mode == Mode::Full && st.buses.empty()) {
            std::vector<int> pins = safe_scan_pins(read_chip_facts().model);
            auto pairs = ordered_pairs(pins);
            pairs_total = pairs.size();
            int64_t start = esp_timer_get_time();
            if (st.cursor >= pairs.size()) st.cursor = 0;
            size_t begin = st.cursor;
            while (st.cursor < pairs.size() && st.buses.size() < kMaxBuses) {
                if (esp_timer_get_time() - start > kScanBudgetUs) {
                    cut_short = true;
                    break;
                }
                const auto& p = pairs[st.cursor++];
                if (in_use(st, p.first) || in_use(st, p.second)) continue;
                if (try_pair(st, p.first, p.second, "scan")) report_bus(st.buses.back(), out);
            }
            pairs_done = st.cursor;
            if (st.cursor >= pairs.size()) st.cursor = 0;  // the next search starts over
            (void)begin;
        }

        if (cut_short) {
            Finding f;
            f.name = "i2c_scan";
            f.verdict = Verdict::Unverified;
            f.evidence = "searched " + std::to_string(pairs_done) + " of " + std::to_string(pairs_total) +
                         " pin pairs in this run; run the self test again to continue";
            out.push_back(std::move(f));
        } else if (out.empty()) {
            Finding f;
            f.name = "i2c_bus";
            f.verdict = Verdict::NotFound;
            if (mode == Mode::Full && pairs_total > 0) {
                f.evidence = "no I2C device answered on " + std::to_string(pairs_total) + " pin pairs";
            } else if (tried_known || !st.known_maps.empty()) {
                f.evidence = "no I2C device at the known pins; run the self test to search";
            } else {
                f.evidence = "no I2C bus known yet; run the self test to search";
            }
            out.push_back(std::move(f));
        }
        return out;
    };
    return d;
}

}  // namespace dc_device::esp32

#endif
