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
    // How often the safety watchdog (Endpoint::tick) runs. 0 disables it.
    // A request that is being handled holds the lock, so a long self test
    // delays the watchdog until it returns.
    uint32_t tick_period_ms = 50;
};

// Serves an Endpoint over ESP-IDF's esp_http_server. The Endpoint must
// outlive the server. On non-ESP builds start() returns false.
//
// The server owns the only lock around the Endpoint: request handlers and the
// watchdog task (which calls Endpoint::tick()) take it, so they never run at
// the same time.
class HttpServer {
public:
    struct Context {
        Endpoint* endpoint = nullptr;
        size_t max_body_bytes = 0;
        void* lock = nullptr;  // SemaphoreHandle_t
    };

    explicit HttpServer(Endpoint& endpoint) { context_.endpoint = &endpoint; }
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer() { stop(); }

    bool start(const HttpServerConfig& config = {});
    void stop();

private:
    Context context_;
    void* server_ = nullptr;  // httpd_handle_t
    void* tick_task_ = nullptr;  // TaskHandle_t
};

}  // namespace dc_device::esp32
