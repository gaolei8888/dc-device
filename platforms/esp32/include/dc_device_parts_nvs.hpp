#pragma once

#include <string>
#include <vector>

#include "dc_device_declared_parts.hpp"

namespace dc_device::esp32 {

// Storage for DeclaredParts in NVS: namespace "dcparts", one byte per part name (0 or 1).
// ESP-IDF only; on a host it returns an empty storage so a host build still links.
PartStorage make_nvs_part_storage(std::vector<std::string> names);

}  // namespace dc_device::esp32
