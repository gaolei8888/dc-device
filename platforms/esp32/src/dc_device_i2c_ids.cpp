#include "dc_device_i2c_ids.hpp"

#include <cstdio>
#include <vector>

namespace dc_device::esp32 {

namespace {

struct RegCheck {
    uint16_t reg;
    int reg_bytes;
    uint8_t expect;
};

struct Chip {
    uint8_t addr;
    const char* kind;
    const char* model;
    // Written before reading (-1 = none): OV2640 register bank select.
    int pre_reg;
    int pre_val;
    std::vector<RegCheck> checks;  // all must match
};

// Identity registers are taken from the chips' datasheets / common drivers.
// Only chips with an id register are here; the rest fall back to the address.
const std::vector<Chip>& chips() {
    static const std::vector<Chip> table = {
        {0x30, "camera", "ov2640", 0xFF, 0x01, {{0x0A, 1, 0x26}, {0x0B, 1, 0x42}}},
        {0x30, "camera", "ov2640", 0xFF, 0x01, {{0x0A, 1, 0x26}, {0x0B, 1, 0x41}}},
        {0x3C, "camera", "ov5640", -1, 0, {{0x300A, 2, 0x56}, {0x300B, 2, 0x40}}},
        {0x68, "imu", "mpu6050", -1, 0, {{0x75, 1, 0x68}}},
        {0x69, "imu", "mpu6050", -1, 0, {{0x75, 1, 0x68}}},
        {0x68, "imu", "mpu6500", -1, 0, {{0x75, 1, 0x70}}},
        {0x68, "imu", "mpu9250", -1, 0, {{0x75, 1, 0x71}}},
        {0x68, "imu", "icm20602", -1, 0, {{0x75, 1, 0x12}}},
        {0x68, "imu", "icm20948", -1, 0, {{0x00, 1, 0xEA}}},
        {0x69, "imu", "icm20948", -1, 0, {{0x00, 1, 0xEA}}},
        {0x76, "environment_sensor", "bme280", -1, 0, {{0xD0, 1, 0x60}}},
        {0x77, "environment_sensor", "bme280", -1, 0, {{0xD0, 1, 0x60}}},
        {0x76, "environment_sensor", "bmp280", -1, 0, {{0xD0, 1, 0x58}}},
        {0x77, "environment_sensor", "bmp280", -1, 0, {{0xD0, 1, 0x58}}},
        {0x18, "audio_codec", "es8311", -1, 0, {{0xFD, 1, 0x83}, {0xFE, 1, 0x11}}},
        {0x19, "audio_codec", "es8311", -1, 0, {{0xFD, 1, 0x83}, {0xFE, 1, 0x11}}},
        {0x38, "touch", "focaltech", -1, 0, {{0xA8, 1, 0x11}}},
        {0x5D, "touch", "gt911", -1, 0, {{0x8140, 2, 0x39}, {0x8141, 2, 0x31}}},
        {0x14, "touch", "gt911", -1, 0, {{0x8140, 2, 0x39}, {0x8141, 2, 0x31}}},
        {0x29, "distance_sensor", "vl53l0x", -1, 0, {{0xC0, 1, 0xEE}}},
        {0x39, "light_sensor", "apds9960", -1, 0, {{0x92, 1, 0xAB}}},
        {0x57, "heart_rate_sensor", "max30102", -1, 0, {{0xFF, 1, 0x15}}},
    };
    return table;
}

// A guess from the address alone. Never presented as fact.
const char* hint_for(uint8_t a) {
    if (a == 0x3C || a == 0x3D) return "could be an OLED display";
    if (a >= 0x20 && a <= 0x27) return "could be an I/O expander";
    if (a >= 0x48 && a <= 0x4B) return "could be an ADC or temperature sensor";
    if (a >= 0x50 && a <= 0x57) return "could be an EEPROM";
    if (a == 0x68 || a == 0x69) return "could be an IMU or a real-time clock";
    if (a == 0x76 || a == 0x77) return "could be a pressure sensor";
    return "";
}

std::string hex2(unsigned v) {
    char b[8];
    snprintf(b, sizeof(b), "0x%02x", v);
    return b;
}

}  // namespace

I2cIdentity identify_i2c(uint8_t addr, const I2cAccess& io) {
    for (const Chip& c : chips()) {
        if (c.addr != addr) continue;
        if (c.pre_reg >= 0 && io.write) {
            if (!io.write(addr, static_cast<uint8_t>(c.pre_reg), static_cast<uint8_t>(c.pre_val))) continue;
        }
        bool match = true;
        std::string seen;
        for (const RegCheck& r : c.checks) {
            uint8_t v = 0;
            if (!io.read || !io.read(addr, r.reg, r.reg_bytes, &v) || v != r.expect) {
                match = false;
                break;
            }
            seen += (seen.empty() ? "" : " ") + hex2(v);
        }
        if (match) {
            I2cIdentity id;
            id.kind = c.kind;
            id.model = c.model;
            id.evidence = std::string(c.model) + ", id " + seen + " at " + hex2(addr);
            id.confirmed = true;
            return id;
        }
    }
    I2cIdentity id;
    id.kind = "i2c";
    id.evidence = "answers at " + hex2(addr) + "; chip not recognised";
    std::string hint = hint_for(addr);
    if (!hint.empty()) id.evidence += " (" + hint + ", a guess from the address)";
    return id;
}

const uint8_t* i2c_discovery_addresses(size_t* count) {
    static std::vector<uint8_t> list;
    if (list.empty()) {
        auto add = [&](uint8_t a) {
            for (uint8_t x : list) {
                if (x == a) return;
            }
            list.push_back(a);
        };
        for (const Chip& c : chips()) add(c.addr);
        for (uint8_t a : {0x3D, 0x40, 0x44, 0x48, 0x50, 0x51, 0x6A, 0x6B, 0x1E, 0x20, 0x21}) add(a);
    }
    *count = list.size();
    return list.data();
}

std::string i2c_finding_name(const I2cIdentity& id, uint8_t addr) {
    if (id.kind == "camera") return "camera";
    return id.kind + "@" + hex2(addr);
}

}  // namespace dc_device::esp32
