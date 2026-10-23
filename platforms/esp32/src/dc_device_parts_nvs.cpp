#include "dc_device_parts_nvs.hpp"

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace dc_device::esp32 {

#ifdef ESP_PLATFORM

PartStorage make_nvs_part_storage(std::vector<std::string> names) {
    PartStorage s;
    s.load = [names] {
        std::map<std::string, bool> out;
        nvs_handle_t h;
        if (nvs_open("dcparts", NVS_READONLY, &h) != ESP_OK) return out;
        for (const auto& name : names) {
            uint8_t value = 0;
            if (nvs_get_u8(h, name.c_str(), &value) == ESP_OK) out[name] = value != 0;
        }
        nvs_close(h);
        return out;
    };
    s.save = [](const std::string& part, bool present) {
        nvs_handle_t h;
        if (nvs_open("dcparts", NVS_READWRITE, &h) != ESP_OK) return false;
        bool ok = nvs_set_u8(h, part.c_str(), present ? 1 : 0) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
        return ok;
    };
    return s;
}

#else

PartStorage make_nvs_part_storage(std::vector<std::string>) { return {}; }

#endif

}  // namespace dc_device::esp32
