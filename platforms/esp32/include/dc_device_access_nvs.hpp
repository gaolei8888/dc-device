#pragma once

#include <cstddef>
#include <cstdint>

#include "dc_device_access.hpp"

namespace dc_device::esp32 {

// Storage for AccessControl in NVS (namespace "dcauth", key "hash", the 32-byte SHA-256
// of the token) and the chip's hardware random generator. ESP-IDF only; on a host these
// return empty/no-op values so a host build still links.
AccessStorage make_nvs_access_storage();
void hardware_random(uint8_t* out, size_t length);

}  // namespace dc_device::esp32
