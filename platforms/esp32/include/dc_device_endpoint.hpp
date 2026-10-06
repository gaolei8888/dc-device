#pragma once

#include <functional>
#include <string>
#include <vector>

#include "dc_device_json.hpp"

namespace dc_device::esp32 {

// Outcome of a capability handler.
struct InvokeResult {
    enum class Status { Ok, InvalidArguments, Failed };

    Status status = Status::Ok;
    std::string result_json = "{}";  // Ok: JSON value placed under "result"
    std::string message;             // InvalidArguments / Failed

    static InvokeResult success(std::string result_json = "{}") {
        return {Status::Ok, std::move(result_json), {}};
    }
    // The caller sent bad arguments (HTTP 400).
    static InvokeResult invalid(std::string message) {
        return {Status::InvalidArguments, {}, std::move(message)};
    }
    // The arguments were fine but the device could not act (HTTP 500).
    static InvokeResult failed(std::string message) {
        return {Status::Failed, {}, std::move(message)};
    }
};

using CapabilityHandler = std::function<InvokeResult(const json::Value& arguments)>;

struct CapabilitySpec {
    std::string name;
    std::string description;
    std::string kind;                         // "sensor" | "action" | "stream"
    std::string parameters_json = "{}";       // JSON Schema object
    std::string returns_json = "{}";          // JSON Schema object
    std::string safety_json = "{}";           // JSON object
    CapabilityHandler handler;
};

struct Response {
    int status = 200;
    std::string body;
};

// Transport-independent implementation of HTTP Transport v0.1
// (docs/http-transport-v0.1.md). It maps (method, path, body) to a response,
// so it can be unit-tested on a host and driven by any HTTP server.
//
// Not thread-safe: the caller must serialize handle() calls. ESP-IDF's
// esp_http_server runs all handlers on a single task, which satisfies this.
class Endpoint {
public:
    Endpoint(std::string id, std::string type, std::string name)
        : id_(std::move(id)), type_(std::move(type)), name_(std::move(name)) {}

    // Returns false (and registers nothing) for an empty/duplicate name, an
    // unknown kind, a missing handler, or a schema field that is not a JSON
    // object.
    bool add(CapabilitySpec spec);

    std::string manifest() const;

    Response handle(const std::string& method,
                    const std::string& target,
                    const std::string& body) const;

private:
    Response invoke(const std::string& body) const;

    std::string id_;
    std::string type_;
    std::string name_;
    std::vector<CapabilitySpec> capabilities_;
};

}  // namespace dc_device::esp32
