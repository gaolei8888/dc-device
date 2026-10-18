#include "dc_device_chip.hpp"

#include "dc_device_endpoint.hpp"
#include "dc_device_json.hpp"

#ifdef ESP_PLATFORM
#include <cstdio>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "sdkconfig.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#endif

namespace dc_device::esp32 {

std::string chip_facts_json(const ChipFacts& f) {
    auto num_or_null = [](long long v) { return v < 0 ? std::string("null") : std::to_string(v); };
    std::string out = "{\"model\":" + json::quote(f.model);
    out += ",\"revision\":" + json::quote(f.revision);
    out += ",\"cores\":" + std::to_string(f.cores);
    out += ",\"features\":[";
    for (size_t i = 0; i < f.features.size(); ++i) {
        if (i) out += ",";
        out += json::quote(f.features[i]);
    }
    out += "],\"flash_bytes\":" + num_or_null(f.flash_bytes);
    out += ",\"psram_bytes\":" + num_or_null(f.psram_bytes);
    out += ",\"mac\":" + json::quote(f.mac);
    out += ",\"idf_version\":" + json::quote(f.idf_version);
    out += ",\"reset_reason\":" + json::quote(f.reset_reason) + "}";
    return out;
}

#ifdef ESP_PLATFORM

static const char* model_name(esp_chip_model_t m) {
    switch (m) {
        case CHIP_ESP32: return "esp32";
        case CHIP_ESP32S2: return "esp32s2";
        case CHIP_ESP32S3: return "esp32s3";
        case CHIP_ESP32C3: return "esp32c3";
        default: return "unknown";
    }
}

static const char* reset_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        default: return "other";
    }
}

ChipFacts read_chip_facts() {
    ChipFacts f;
    esp_chip_info_t info{};
    esp_chip_info(&info);
    f.model = model_name(info.model);
    char rev[16];
    snprintf(rev, sizeof(rev), "v%d.%d", info.revision / 100, info.revision % 100);
    f.revision = rev;
    f.cores = info.cores;
    if (info.features & CHIP_FEATURE_WIFI_BGN) f.features.push_back("wifi");
    if (info.features & CHIP_FEATURE_BLE) f.features.push_back("ble");
    if (info.features & CHIP_FEATURE_BT) f.features.push_back("bt");
    if (info.features & CHIP_FEATURE_EMB_FLASH) f.features.push_back("embedded_flash");
    uint32_t flash = 0;
    if (esp_flash_get_size(nullptr, &flash) == ESP_OK) f.flash_bytes = flash;
#if CONFIG_SPIRAM
    f.psram_bytes = static_cast<long long>(esp_psram_get_size());
#endif
    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        char s[13];
        snprintf(s, sizeof(s), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        f.mac = s;
    }
    f.idf_version = esp_get_idf_version();
    f.reset_reason = reset_name(esp_reset_reason());
    return f;
}

bool register_chip_diagnostics(Endpoint& endpoint) {
    return endpoint.add_diagnostic("chip", [] { return chip_facts_json(read_chip_facts()); });
}

#endif

}  // namespace dc_device::esp32
