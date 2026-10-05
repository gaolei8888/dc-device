#pragma once

#include <string>

namespace dc_device::esp32 {

struct WifiConfig {
    std::string ssid;
    std::string password;
    int max_retries = 10;
};

bool connect_wifi(const WifiConfig& config);

}  // namespace dc_device::esp32
