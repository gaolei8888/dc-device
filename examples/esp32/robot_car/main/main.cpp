#include <cstring>
#include <string>

#include "dc_device_esp32.hpp"

#ifdef ESP_PLATFORM
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#endif

using dc_device::esp32::GpioOutput;
using dc_device::esp32::Servo;

static GpioOutput status_led(2);
static Servo pan_servo(18, 0);

#ifdef ESP_PLATFORM

static const char* TAG = "dc-device";

static esp_err_t health_handler(httpd_req_t* req) {
    const char* body = R"({"status":"ok","device_id":"robot_car_01"})";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t manifest_handler(httpd_req_t* req) {
    const char* body = R"json(
{
  "spec_version":"0.1",
  "device":{
    "id":"robot_car_01",
    "type":"mobile_robot",
    "name":"ESP32-S3 Robot Car"
  },
  "capabilities":[
    {
      "name":"led.set",
      "description":"Set the status LED.",
      "kind":"action",
      "parameters":{
        "type":"object",
        "properties":{"on":{"type":"boolean"}},
        "required":["on"]
      }
    },
    {
      "name":"servo.pan",
      "description":"Rotate the camera pan servo.",
      "kind":"action",
      "parameters":{
        "type":"object",
        "properties":{"angle":{"type":"number","minimum":-90,"maximum":90}},
        "required":["angle"]
      }
    }
  ]
}
)json";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static std::string read_body(httpd_req_t* req) {
    std::string body;
    body.resize(req->content_len);
    int received = httpd_req_recv(req, body.data(), body.size());
    if (received <= 0) {
        return {};
    }
    body.resize(received);
    return body;
}

static esp_err_t invoke_handler(httpd_req_t* req) {
    std::string body = read_body(req);
    httpd_resp_set_type(req, "application/json");

    if (body.find("\"capability\":\"led.set\"") != std::string::npos ||
        body.find("\"capability\": \"led.set\"") != std::string::npos) {
        bool on = body.find("\"on\":true") != std::string::npos ||
                  body.find("\"on\": true") != std::string::npos;
        status_led.write(on);
        const char* response = on
            ? R"({"ok":true,"capability":"led.set","result":{"on":true}})"
            : R"({"ok":true,"capability":"led.set","result":{"on":false}})";
        return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    }

    if (body.find("\"capability\":\"servo.pan\"") != std::string::npos ||
        body.find("\"capability\": \"servo.pan\"") != std::string::npos) {
        auto pos = body.find("\"angle\":");
        if (pos == std::string::npos) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, R"({"ok":false,"error":"missing angle"})");
        }
        pos += std::strlen("\"angle\":");
        float angle = std::stof(body.substr(pos));
        if (!pan_servo.write_angle(angle)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, R"({"ok":false,"error":"invalid angle"})");
        }
        std::string response =
            std::string(R"({"ok":true,"capability":"servo.pan","result":{"angle":)") +
            std::to_string(angle) + "}}";
        return httpd_resp_send(req, response.c_str(), response.size());
    }

    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, R"({"ok":false,"error":"unknown capability"})");
}

static void start_http_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8787;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t health{
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &health);

    httpd_uri_t manifest{
        .uri = "/manifest",
        .method = HTTP_GET,
        .handler = manifest_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &manifest);

    httpd_uri_t invoke{
        .uri = "/invoke",
        .method = HTTP_POST,
        .handler = invoke_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &invoke);

    ESP_LOGI(TAG, "dc-device HTTP server listening on port 8787");
}

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    status_led.begin();
    pan_servo.begin();

    // Wi-Fi initialization is board/application-specific in v0.1.
    // Connect Wi-Fi before starting the HTTP server.
    start_http_server();
}

#else

int main() {
    status_led.begin();
    pan_servo.begin();
    status_led.write(true);
    pan_servo.write_angle(30);
    return 0;
}

#endif
