#ifdef ESP_PLATFORM

#include "dc_device_camera_detect.hpp"

#include <cstdio>
#include <memory>

#include "dc_device_chip.hpp"
#include "dc_device_pins.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

namespace dc_device::esp32 {

namespace {

constexpr int kProbeTimeoutMs = 20;
constexpr int kXferTimeoutMs = 50;

// The bus stays open once a camera answered on it: building and tearing down a
// bus on every /status glitched the sensor's lines and, after a probe timeout,
// hung the I2C driver until the watchdog reset the chip.
struct CameraState {
    int sda = -1;
    int scl = -1;
    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_dev_handle_t dev = nullptr;
    uint8_t addr = 0;  // 0x30 (OV2640) or 0x3C (OV5640)
    std::vector<PinPair> known_maps;
};

void close_bus(CameraState& st, bool reset_pins) {
    if (st.dev) i2c_master_bus_rm_device(st.dev);
    if (st.bus) i2c_del_master_bus(st.bus);
    st.dev = nullptr;
    st.bus = nullptr;
    if (reset_pins && st.sda >= 0) {
        gpio_reset_pin(static_cast<gpio_num_t>(st.sda));
        gpio_reset_pin(static_cast<gpio_num_t>(st.scl));
    }
}

bool open_bus(CameraState& st, int sda, int scl) {
    i2c_master_bus_config_t cfg{};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = static_cast<gpio_num_t>(sda);
    cfg.scl_io_num = static_cast<gpio_num_t>(scl);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    st.sda = sda;
    st.scl = scl;
    return i2c_new_master_bus(&cfg, &st.bus) == ESP_OK;
}

bool add_dev(CameraState& st, uint8_t addr) {
    i2c_device_config_t dc{};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = 100000;
    st.addr = addr;
    return i2c_master_bus_add_device(st.bus, &dc, &st.dev) == ESP_OK;
}

bool read8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* out) {
    return i2c_master_transmit_receive(dev, &reg, 1, out, 1, kXferTimeoutMs) == ESP_OK;
}

bool read16addr(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t* out) {
    uint8_t a[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)};
    return i2c_master_transmit_receive(dev, a, 2, out, 1, kXferTimeoutMs) == ESP_OK;
}

// Reads the chip id with the device already added at st.addr. "" if it is not
// the expected sensor.
std::string read_id(CameraState& st) {
    char buf[48];
    uint8_t hi = 0, lo = 0;
    if (st.addr == 0x30) {
        uint8_t sel[2] = {0xFF, 0x01};  // register bank 1
        if (i2c_master_transmit(st.dev, sel, 2, kXferTimeoutMs) == ESP_OK && read8(st.dev, 0x0A, &hi) &&
            read8(st.dev, 0x0B, &lo) && hi == 0x26 && (lo == 0x41 || lo == 0x42)) {
            snprintf(buf, sizeof(buf), "ov2640, chip id 0x%02x%02x at 0x30", hi, lo);
            return buf;
        }
    } else if (st.addr == 0x3C) {
        if (read16addr(st.dev, 0x300A, &hi) && read16addr(st.dev, 0x300B, &lo) && hi == 0x56 && lo == 0x40) {
            snprintf(buf, sizeof(buf), "ov5640, chip id 0x%02x%02x at 0x3c", hi, lo);
            return buf;
        }
    }
    return "";
}

// Opens a bus on (sda, scl) and looks for a camera. On success the bus and the
// device stay open in `st`. On failure everything is closed and the pins are
// returned to plain inputs. A probe that times out ends the attempt at once.
std::string find_camera(CameraState& st, int sda, int scl) {
    if (!open_bus(st, sda, scl)) {
        st.bus = nullptr;
        return "";
    }
    for (uint8_t addr : {uint8_t{0x30}, uint8_t{0x3C}}) {
        esp_err_t e = i2c_master_probe(st.bus, addr, kProbeTimeoutMs);
        if (e == ESP_ERR_TIMEOUT) break;
        if (e != ESP_OK) continue;
        if (add_dev(st, addr)) {
            std::string id = read_id(st);
            if (!id.empty()) return id;
            i2c_master_bus_rm_device(st.dev);
            st.dev = nullptr;
        }
    }
    close_bus(st, true);
    return "";
}

std::string wiring_text(int sda, int scl) {
    return "SDA " + std::to_string(sda) + ", SCL " + std::to_string(scl);
}

}  // namespace

Detector make_camera_detector(std::vector<PinPair> known_maps) {
    auto state = std::make_shared<CameraState>();
    state->known_maps = std::move(known_maps);

    Detector d;
    d.name = "camera";
    d.klass = PeripheralClass::Bus;
    d.run = [state](Mode mode) {
        CameraState& st = *state;
        Finding f;
        auto found = [&](const std::string& id, const char* source) {
            f.verdict = Verdict::Ok;
            f.evidence = id;
            f.wiring = wiring_text(st.sda, st.scl);
            f.source = source;
            return f;
        };
        // Already open: just read the id again.
        if (st.bus != nullptr && st.dev != nullptr) {
            std::string id = read_id(st);
            if (!id.empty()) return found(id, "");
            f.verdict = Verdict::Failed;
            f.evidence = "the camera found earlier on " + wiring_text(st.sda, st.scl) + " no longer answers";
            f.wiring = wiring_text(st.sda, st.scl);
            close_bus(st, true);  // the next run starts again from the known maps
            return f;
        }
        for (const PinPair& m : st.known_maps) {
            std::string id = find_camera(st, m.sda, m.scl);
            if (!id.empty()) return found(id, "known_map");
        }
        if (mode != Mode::Full) {
            f.verdict = Verdict::NotFound;
            f.evidence = "not at any known pins; run the self test to search";
            return f;
        }
        std::vector<int> pins = safe_scan_pins(read_chip_facts().model);
        if (pins.empty()) {
            f.verdict = Verdict::NotFound;
            f.evidence = "not at any known pins, and this chip has no safe pin table to search";
            return f;
        }
        int tried = 0;
        for (const auto& p : ordered_pairs(pins)) {
            ++tried;
            std::string id = find_camera(st, p.first, p.second);
            if (!id.empty()) return found(id, "scan");
        }
        f.verdict = Verdict::NotFound;
        f.evidence = "no camera answered on " + std::to_string(tried) + " pin pairs";
        return f;
    };
    return d;
}

}  // namespace dc_device::esp32

#endif
