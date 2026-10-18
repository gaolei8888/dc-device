#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace dc_device::esp32 {

// Reads and writes registers of the device at `addr`. Supplied by the platform;
// a test supplies a fake. Both return false when the device does not answer.
struct I2cAccess {
    // reg_bytes is 1 or 2 (the width of the register address).
    std::function<bool(uint8_t addr, uint16_t reg, int reg_bytes, uint8_t* out)> read;
    std::function<bool(uint8_t addr, uint8_t reg, uint8_t value)> write;
};

struct I2cIdentity {
    std::string kind;      // "camera", "imu", "audio_codec", ... or "i2c" if unknown
    std::string model;     // "ov2640", "mpu6050", ... or ""
    std::string evidence;  // what was read, e.g. "WHO_AM_I 0x68 at 0x68"
    bool confirmed = false;  // an identity register matched (not just an address)
};

// Identifies the device that answered at `addr`. Reads identity registers from a
// small table of known chips; the only write is the OV2640 register-bank select
// (0xFF = 0x01), needed to read its id. A device whose registers match nothing is
// reported as kind "i2c", confirmed = false, with a hint taken from the address
// ("could be an OLED display") that is a guess and says so.
I2cIdentity identify_i2c(uint8_t addr, const I2cAccess& io);

// Addresses worth probing when looking for a bus: every address the table knows.
const uint8_t* i2c_discovery_addresses(size_t* count);

// Finding name for a device: "camera" for a camera sensor, otherwise
// "<kind>@0x<addr>" (stable across runs, unique per bus address).
std::string i2c_finding_name(const I2cIdentity& id, uint8_t addr);

}  // namespace dc_device::esp32
