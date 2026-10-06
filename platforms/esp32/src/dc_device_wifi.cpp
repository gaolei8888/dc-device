#include "dc_device_wifi.hpp"

#ifdef ESP_PLATFORM
#include <cstring>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#endif

namespace dc_device::esp32 {

#ifdef ESP_PLATFORM
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static int s_max_retries = 10;
static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr int WIFI_FAIL_BIT = BIT1;
static const char* TAG = "dc-device-wifi";

static void event_handler(void*, esp_event_base_t event_base, int32_t event_id, void*) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < s_max_retries) {
            esp_wifi_connect();
            ++s_retry_num;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
#endif

bool connect_wifi(const WifiConfig& config) {
#ifdef ESP_PLATFORM
    s_wifi_event_group = xEventGroupCreate();
    s_max_retries = config.max_retries;

    // Without a default STA netif the interface never gets an IP.
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    if (esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, &instance_any_id) != ESP_OK) {
        return false;
    }
    if (esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, &instance_got_ip) != ESP_OK) {
        return false;
    }

    wifi_config_t wifi_config{};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                 config.ssid.c_str(),
                 sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                 config.password.c_str(),
                 sizeof(wifi_config.sta.password) - 1);

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    bool connected = (bits & WIFI_CONNECTED_BIT) != 0;
    ESP_LOGI(TAG, connected ? "Wi-Fi connected" : "Wi-Fi connection failed");
    return connected;
#else
    (void)config;
    return true;
#endif
}

}  // namespace dc_device::esp32
