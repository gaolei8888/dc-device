#include <string>

#include "dc_device_endpoint.hpp"
#include "dc_device_esp32.hpp"
#include "dc_device_http_server.hpp"
#include "dc_device_wifi.hpp"

#ifdef ESP_PLATFORM
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#endif

using dc_device::esp32::Endpoint;
using dc_device::esp32::GpioOutput;
using dc_device::esp32::InvokeResult;
using dc_device::esp32::Servo;
namespace json = dc_device::json;

static GpioOutput status_led(2);
static Servo pan_servo(18, 0);
static Endpoint endpoint("robot_car_01", "mobile_robot", "ESP32-S3 Robot Car");

static InvokeResult led_set(const json::Value& args) {
    const json::Value* on = args.find("on");
    if (on == nullptr) {
        return InvokeResult::invalid("Missing required argument: on");
    }
    if (!on->is_bool()) {
        return InvokeResult::invalid("Argument on must be of type boolean");
    }
    if (!status_led.write(on->boolean)) {
        return InvokeResult::failed("Could not drive the status LED");
    }
    return InvokeResult::success(std::string("{\"on\":") + (on->boolean ? "true" : "false") + "}");
}

static InvokeResult servo_pan(const json::Value& args) {
    const json::Value* angle = args.find("angle");
    if (angle == nullptr) {
        return InvokeResult::invalid("Missing required argument: angle");
    }
    if (!angle->is_number()) {
        return InvokeResult::invalid("Argument angle must be of type number");
    }
    if (angle->number < -90) {
        return InvokeResult::invalid("Argument angle must be >= -90");
    }
    if (angle->number > 90) {
        return InvokeResult::invalid("Argument angle must be <= 90");
    }
    if (!pan_servo.write_angle(static_cast<float>(angle->number))) {
        return InvokeResult::failed("Could not drive the pan servo");
    }
    return InvokeResult::success("{\"angle\":" + json::number(angle->number) + "}");
}

static void register_capabilities() {
    dc_device::esp32::CapabilitySpec led;
    led.name = "led.set";
    led.description = "Set the status LED.";
    led.kind = "action";
    led.parameters_json =
        R"({"type":"object","properties":{"on":{"type":"boolean"}},"required":["on"]})";
    led.handler = led_set;
    endpoint.add(led);

    dc_device::esp32::CapabilitySpec servo;
    servo.name = "servo.pan";
    servo.description = "Rotate the camera pan servo.";
    servo.kind = "action";
    servo.parameters_json =
        R"({"type":"object","properties":{"angle":{"type":"number","minimum":-90,"maximum":90}},"required":["angle"]})";
    servo.handler = servo_pan;
    endpoint.add(servo);
}

#ifdef ESP_PLATFORM

static const char* TAG = "dc-device";

static dc_device::esp32::HttpServer http_server(endpoint);

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    status_led.begin();
    pan_servo.begin();
    register_capabilities();

    dc_device::esp32::WifiConfig wifi;
    wifi.ssid = CONFIG_DC_DEVICE_WIFI_SSID;
    wifi.password = CONFIG_DC_DEVICE_WIFI_PASSWORD;
    if (wifi.ssid.empty()) {
        ESP_LOGE(TAG, "No Wi-Fi SSID set. Run `idf.py menuconfig` -> dc-device robot car.");
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

#else

// Host build: exercises the same capability handlers without hardware.
int main() {
    status_led.begin();
    pan_servo.begin();
    register_capabilities();
    auto response = endpoint.handle(
        "POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":30}})");
    return response.status == 200 ? 0 : 1;
}

#endif
