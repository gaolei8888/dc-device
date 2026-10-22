#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dc_device {

// SHA-256 (FIPS 180-4). Portable, so the access token code runs in host tests; the
// board does not need a second implementation for 32 bytes of input.
std::array<uint8_t, 32> sha256(const void* data, size_t length);

}  // namespace dc_device
