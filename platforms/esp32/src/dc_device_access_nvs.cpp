#include "dc_device_access_nvs.hpp"

#ifdef ESP_PLATFORM
#include "esp_random.h"
#include "nvs.h"
#endif

namespace dc_device::esp32 {

#ifdef ESP_PLATFORM

namespace {
const char* kNamespace = "dcauth";
const char* kKey = "hash";
}  // namespace

AccessStorage make_nvs_access_storage() {
    AccessStorage s;
    s.load = [](std::array<uint8_t, 32>& hash) {
        nvs_handle_t h;
        esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &h);
        if (opened == ESP_ERR_NVS_NOT_FOUND) return LoadResult::NotFound;  // namespace never written
        if (opened != ESP_OK) return LoadResult::Error;
        size_t length = hash.size();
        esp_err_t got = nvs_get_blob(h, kKey, hash.data(), &length);
        nvs_close(h);
        if (got == ESP_ERR_NVS_NOT_FOUND) return LoadResult::NotFound;
        return got == ESP_OK && length == hash.size() ? LoadResult::Found : LoadResult::Error;
    };
    s.save = [](const std::array<uint8_t, 32>& hash) {
        nvs_handle_t h;
        if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
        bool ok = nvs_set_blob(h, kKey, hash.data(), hash.size()) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
        return ok;
    };
    s.erase = []() {
        nvs_handle_t h;
        if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
        esp_err_t e = nvs_erase_key(h, kKey);
        bool ok = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
        nvs_close(h);
        return ok;
    };
    return s;
}

void hardware_random(uint8_t* out, size_t length) { esp_fill_random(out, length); }

#else

AccessStorage make_nvs_access_storage() { return {}; }
void hardware_random(uint8_t* out, size_t length) {
    for (size_t i = 0; i < length; ++i) out[i] = 0;  // never used for tokens off the board
}

#endif

}  // namespace dc_device::esp32
