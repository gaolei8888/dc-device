#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dc_device {

// Standard base64 (RFC 4648 section 4) with padding, no line breaks.
std::string base64_encode(const uint8_t* data, size_t length);

// Strict: the text must be whole four-character groups from the standard alphabet with
// correct padding. Whitespace, URL-safe characters, missing or extra padding and any other
// byte are refused. On failure `out` is left empty and false is returned.
bool base64_decode(const std::string& text, std::vector<uint8_t>& out);

}  // namespace dc_device
