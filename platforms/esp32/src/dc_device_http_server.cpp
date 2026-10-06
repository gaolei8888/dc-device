#include "dc_device_http_server.hpp"

#include <string>

#ifdef ESP_PLATFORM
#include "esp_http_server.h"
#include "esp_log.h"
#endif

namespace dc_device::esp32 {

#ifdef ESP_PLATFORM

namespace {

const char* TAG = "dc-device-http";
constexpr int kMaxRecvTimeouts = 3;

// httpd_resp_set_status() keeps the pointer until the response is sent, so
// these must be static strings.
const char* status_line(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 404: return "404 Not Found";
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
    Response response = ctx->endpoint->handle(method, req->uri, body);
    return send(req, response.status, response.body);
}

}  // namespace

bool HttpServer::start(const HttpServerConfig& config) {
    if (server_ != nullptr) {
        return true;
    }
    context_.max_body_bytes = config.max_body_bytes;

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = config.port;
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
    ESP_LOGI(TAG, "dc-device HTTP server listening on port %u", config.port);
    return true;
}

void HttpServer::stop() {
    if (server_ != nullptr) {
        httpd_stop(static_cast<httpd_handle_t>(server_));
        server_ = nullptr;
    }
}

#else

bool HttpServer::start(const HttpServerConfig&) { return false; }
void HttpServer::stop() { server_ = nullptr; }

#endif

}  // namespace dc_device::esp32
