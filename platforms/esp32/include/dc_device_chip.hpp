#pragma once

#include <string>
#include <vector>

namespace dc_device::esp32 {

class Endpoint;

// What the chip itself can report without any peripheral wired
// (docs/self-check-v0.1.md, level 1). Always read-only and cheap.
// A value of -1 means "this firmware cannot tell"; it is reported as null
// and is not the same as zero.
struct ChipFacts {
    std::string model;       // "esp32s3"
    std::string revision;    // "v0.2"
    int cores = 0;
    std::vector<std::string> features;  // "wifi", "ble", ...
    long long flash_bytes = -1;
    long long psram_bytes = -1;
    std::string mac;         // 12 upper-case hex digits, no separators
    std::string idf_version;
    std::string reset_reason;
};

// Platform independent: renders the facts as one JSON object.
std::string chip_facts_json(const ChipFacts& facts);

#ifdef ESP_PLATFORM
// Reads the facts from the chip. PSRAM is reported only when the firmware was
// built with it enabled; otherwise psram_bytes stays -1 (the chip may have
// PSRAM the firmware does not use, and this cannot tell).
ChipFacts read_chip_facts();

// Registers a "chip" entry in GET /diagnostics.
bool register_chip_diagnostics(Endpoint& endpoint);
#endif

}  // namespace dc_device::esp32
