#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <utility>
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

// Outcome of one health check (docs/health-contract-v0.1.md).
struct CheckResult {
    enum class State { Ok, Warning, Failed, Skipped };

    State state = State::Ok;
    std::string code;     // issue code for Warning / Failed
    std::string message;  // issue message for Warning / Failed

    static CheckResult ok() { return {}; }
    static CheckResult skipped() { return {State::Skipped, {}, {}}; }
    static CheckResult warning(std::string code, std::string message) {
        return {State::Warning, std::move(code), std::move(message)};
    }
    static CheckResult failed(std::string code, std::string message) {
        return {State::Failed, std::move(code), std::move(message)};
    }
};

using HealthCheck = std::function<CheckResult()>;
// Returns any JSON value; invalid JSON is reported as an error entry.
using DiagnosticProvider = std::function<std::string()>;

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

    // Health contract. Names are unique across health checks and self tests.
    // Health checks must be cheap and read-only; self tests may actuate.
    bool add_health_check(std::string name, HealthCheck check);
    bool add_self_test(std::string name, HealthCheck check);
    bool add_diagnostic(std::string name, DiagnosticProvider provider);

    std::string manifest() const;
    std::string status() const;       // health checks only
    std::string self_test() const;    // health checks + self tests
    std::string diagnostics() const;  // read-only, never runs a self test

    Response handle(const std::string& method,
                    const std::string& target,
                    const std::string& body) const;

private:
    struct NamedCheck {
        std::string name;
        HealthCheck check;
    };

    Response invoke(const std::string& body) const;
    bool name_taken(const std::string& name) const;
    std::string report(const std::vector<const NamedCheck*>& checks, std::string* status_out) const;

    std::string id_;
    std::string type_;
    std::string name_;
    std::vector<CapabilitySpec> capabilities_;
    std::vector<NamedCheck> health_checks_;
    std::vector<NamedCheck> self_tests_;
    std::vector<std::pair<std::string, DiagnosticProvider>> diagnostics_;
    mutable std::string last_self_test_;  // JSON report, empty until one runs
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

}  // namespace dc_device::esp32
