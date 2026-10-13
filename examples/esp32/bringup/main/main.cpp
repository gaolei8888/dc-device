// Bring-up firmware: joins Wi-Fi and serves the dc-device HTTP contract, and
// drives no GPIO pin at all. For a board whose wiring is not known (for
// example a camera board). Its only capability reads the free heap.
#include <cstdio>
#include <string>

#include "dc_device_endpoint.hpp"
#include "dc_device_http_server.hpp"
#include "dc_device_wifi.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

using dc_device::esp32::CheckResult;
using dc_device::esp32::Endpoint;
using dc_device::esp32::InvokeResult;
namespace json = dc_device::json;

static const char* TAG = "dc-device";
static Endpoint endpoint("bringup_01", "bringup_board", "ESP32-S3 Bring-up");
static dc_device::esp32::HttpServer http_server(endpoint);

static InvokeResult heap_read(const json::Value&) {
    return InvokeResult::success(std::string("{\"free_heap_bytes\":") +
                                 std::to_string(esp_get_free_heap_size()) + "}");
}

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    dc_device::esp32::CapabilitySpec heap;
    heap.name = "heap.read";
    heap.description = "Read the free heap size.";
    heap.kind = "sensor";
    heap.parameters_json = R"({"type":"object","properties":{}})";
    heap.handler = heap_read;
    endpoint.add(heap);

    dc_device::esp32::Identity id;
    id.model = "ESP32-S3 Bring-up";
    id.firmware_version = "0.1.0";
    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        char serial[13];
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        id.serial = serial;
    }
    endpoint.set_identity(std::move(id));
    endpoint.add_diagnostic("free_heap_bytes", [] { return std::to_string(esp_get_free_heap_size()); });

    dc_device::esp32::WifiConfig wifi;
    wifi.ssid = CONFIG_DC_DEVICE_WIFI_SSID;
    wifi.password = CONFIG_DC_DEVICE_WIFI_PASSWORD;
    if (wifi.ssid.empty()) {
        ESP_LOGE(TAG, "No Wi-Fi SSID set");
        return;
    }
    if (!dc_device::esp32::connect_wifi(wifi)) {
        ESP_LOGE(TAG, "Wi-Fi connection failed; HTTP server not started");
        return;
    }
    dc_device::esp32::HttpServerConfig http;
    http.port = CONFIG_DC_DEVICE_HTTP_PORT;
    http_server.start(http);
}
