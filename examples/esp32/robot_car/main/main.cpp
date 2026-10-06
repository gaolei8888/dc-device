#include <string>

#include "dc_device_endpoint.hpp"
#include "dc_device_esp32.hpp"
#include "dc_device_http_server.hpp"
#include "dc_device_wifi.hpp"

#ifdef ESP_PLATFORM
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#endif

using dc_device::esp32::CheckResult;
using dc_device::esp32::Endpoint;
using dc_device::esp32::GpioOutput;
using dc_device::esp32::InvokeResult;
using dc_device::esp32::Servo;
namespace json = dc_device::json;

static bool led_ready = false;
static bool servo_ready = false;
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

static void pause_ms(int ms) {
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    (void)ms;
#endif
}

// Cheap, read-only: did the hardware initialise at boot?
static CheckResult check_led() {
    return led_ready ? CheckResult::ok()
                     : CheckResult::failed("LED_INIT_FAILED", "Status LED GPIO failed to initialise");
}

static CheckResult check_servo() {
    return servo_ready ? CheckResult::ok()
                       : CheckResult::failed("SERVO_INIT_FAILED", "Pan servo PWM failed to initialise");
}

// Actuating: blinks the LED, leaves it off.
static CheckResult selftest_led_blink() {
    if (!led_ready) return CheckResult::skipped();
    for (int i = 0; i < 2; ++i) {
        if (!status_led.write(true)) return CheckResult::failed("LED_WRITE_FAILED", "Could not turn LED on");
        pause_ms(150);
        if (!status_led.write(false)) return CheckResult::failed("LED_WRITE_FAILED", "Could not turn LED off");
        pause_ms(150);
    }
    return CheckResult::ok();
}

// Actuating: sweeps the pan servo and leaves it centred.
static CheckResult selftest_servo_sweep() {
    if (!servo_ready) return CheckResult::skipped();
    for (float angle : {-30.0f, 30.0f, 0.0f}) {
        if (!pan_servo.write_angle(angle)) {
            return CheckResult::failed("SERVO_WRITE_FAILED", "Could not drive the pan servo");
        }
        pause_ms(400);
    }
    return CheckResult::ok();
}

static void register_health() {
    endpoint.add_health_check("led", check_led);
    endpoint.add_health_check("servo", check_servo);
    endpoint.add_self_test("led_blink", selftest_led_blink);
    endpoint.add_self_test("servo_sweep", selftest_servo_sweep);
#ifdef ESP_PLATFORM
    endpoint.add_diagnostic("free_heap_bytes", [] { return std::to_string(esp_get_free_heap_size()); });
    endpoint.add_diagnostic("min_free_heap_bytes",
                            [] { return std::to_string(esp_get_minimum_free_heap_size()); });
#endif
}

#ifdef ESP_PLATFORM

static const char* TAG = "dc-device";

static dc_device::esp32::HttpServer http_server(endpoint);

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    led_ready = status_led.begin();
    servo_ready = pan_servo.begin();
    register_capabilities();
    register_health();

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
    led_ready = status_led.begin();
    servo_ready = pan_servo.begin();
    register_capabilities();
    register_health();
    auto response = endpoint.handle(
        "POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":30}})");
    auto self_test = endpoint.handle("POST", "/self_test", "");
    return response.status == 200 && self_test.body.find("\"status\":\"ok\"") != std::string::npos ? 0 : 1;
}

#endif
