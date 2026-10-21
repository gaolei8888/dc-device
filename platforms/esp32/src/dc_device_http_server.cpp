#include "dc_device_http_server.hpp"

#include <string>

#ifdef ESP_PLATFORM
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace dc_device::esp32 {

#ifdef ESP_PLATFORM

namespace {

const char* TAG = "dc-device-http";
constexpr uint32_t kTickStack = 4096;  // runs the safe-state handler
constexpr int kMaxRecvTimeouts = 3;

// httpd_resp_set_status() keeps the pointer until the response is sent, so
// these must be static strings.
const char* status_line(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 404: return "404 Not Found";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        default:  return "500 Internal Server Error";
    }
}

esp_err_t send(httpd_req_t* req, int status, const std::string& body) {
    httpd_resp_set_status(req, status_line(status));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t dispatch(httpd_req_t* req) {
    const auto* ctx = static_cast<const HttpServer::Context*>(req->user_ctx);

    if (req->content_len > ctx->max_body_bytes) {
        // The unread body would corrupt keep-alive framing, so close.
        httpd_resp_set_hdr(req, "Connection", "close");
        return send(req, 413,
                    R"({"ok":false,"error":"PayloadTooLarge","message":"Request body too large"})");
    }

    std::string body(req->content_len, '\0');
    size_t received = 0;
    int timeouts = 0;
    while (received < body.size()) {
        int n = httpd_req_recv(req, &body[received], body.size() - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kMaxRecvTimeouts) {
            continue;
        }
        if (n <= 0) {
            // Connection is unusable; let httpd close it.
            return ESP_FAIL;
        }
        received += static_cast<size_t>(n);
    }

    const char* method = req->method == HTTP_GET    ? "GET"
                         : req->method == HTTP_POST ? "POST"
                                                    : "";
    // One request at a time, and never while the watchdog is acting.
    auto lock = static_cast<SemaphoreHandle_t>(ctx->lock);
    xSemaphoreTake(lock, portMAX_DELAY);
    Response response = ctx->endpoint->handle(method, req->uri, body);
    xSemaphoreGive(lock);
    return send(req, response.status, response.body);
}

struct TickArgs {
    HttpServer::Context* context;
    uint32_t period_ms;
};

void tick_task(void* raw) {
    auto* args = static_cast<TickArgs*>(raw);
    auto lock = static_cast<SemaphoreHandle_t>(args->context->lock);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(args->period_ms));
        // A request in progress holds the lock; try again next period.
        if (xSemaphoreTake(lock, pdMS_TO_TICKS(5)) != pdTRUE) continue;
        bool acted = args->context->endpoint->tick();
        xSemaphoreGive(lock);
        if (acted) ESP_LOGW(TAG, "time limit passed: safe state ran");
    }
}

}  // namespace

bool HttpServer::start(const HttpServerConfig& config) {
    if (server_ != nullptr) {
        return true;
    }
    context_.max_body_bytes = config.max_body_bytes;
    if (context_.lock == nullptr) {
        context_.lock = xSemaphoreCreateMutex();
        if (context_.lock == nullptr) return false;
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = config.port;
    httpd_config.stack_size = config.stack_size;
    // Route everything through Endpoint::handle() so 404s and the JSON error
    // shape come from one place.
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t handle = nullptr;
    if (httpd_start(&handle, &httpd_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %u", config.port);
        return false;
    }

    for (httpd_method_t method : {HTTP_GET, HTTP_POST}) {
        httpd_uri_t uri{};
        uri.uri = "/*";
        uri.method = method;
        uri.handler = dispatch;
        uri.user_ctx = &context_;
        if (httpd_register_uri_handler(handle, &uri) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI handler");
            httpd_stop(handle);
            return false;
        }
    }

    server_ = handle;
    if (config.tick_period_ms > 0 && tick_task_ == nullptr) {
        static TickArgs args;  // one server per device
        args = {&context_, config.tick_period_ms};
        TaskHandle_t task = nullptr;
        if (xTaskCreate(tick_task, "dc_tick", kTickStack, &args, 5, &task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start the safety watchdog task");
            httpd_stop(handle);
            server_ = nullptr;
            return false;
        }
        tick_task_ = task;
    }
    ESP_LOGI(TAG, "dc-device HTTP server listening on port %u", config.port);
    return true;
}

void HttpServer::pause() {
    if (server_ != nullptr) {
        httpd_stop(static_cast<httpd_handle_t>(server_));
        server_ = nullptr;
    }
}

void HttpServer::stop() {
    pause();
    if (tick_task_ != nullptr) {
        vTaskDelete(static_cast<TaskHandle_t>(tick_task_));
        tick_task_ = nullptr;
    }
}

#else

bool HttpServer::start(const HttpServerConfig&) { return false; }
void HttpServer::pause() { server_ = nullptr; }
void HttpServer::stop() { server_ = nullptr; }

#endif

}  // namespace dc_device::esp32
