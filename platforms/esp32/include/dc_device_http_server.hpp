#pragma once

#include <cstddef>
#include <cstdint>

#include "dc_device_endpoint.hpp"

namespace dc_device::esp32 {

struct HttpServerConfig {
    uint16_t port = 8787;
    size_t max_body_bytes = 2048;  // larger POST bodies get 413
    // Stack of the server task. Capability handlers, health checks and self tests
    // all run on it; 4096 (the IDF default) overflowed in a driver call.
    size_t stack_size = 8192;
};

// Serves an Endpoint over ESP-IDF's esp_http_server. The Endpoint must
// outlive the server. On non-ESP builds start() returns false.
class HttpServer {
public:
    struct Context {
        const Endpoint* endpoint = nullptr;
        size_t max_body_bytes = 0;
    };

    explicit HttpServer(const Endpoint& endpoint) { context_.endpoint = &endpoint; }
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer() { stop(); }

    bool start(const HttpServerConfig& config = {});
    void stop();

private:
    Context context_;
    void* server_ = nullptr;  // httpd_handle_t
};

}  // namespace dc_device::esp32
