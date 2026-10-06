#include "dc_device_endpoint.hpp"

namespace dc_device::esp32 {

namespace {

bool is_json_object(const std::string& text) {
    json::Value value;
    std::string error;
    return json::parse(text, value, error) && value.is_object();
}

std::string error_body(const std::string& code, const std::string& message) {
    return "{\"ok\":false,\"error\":" + json::quote(code) +
           ",\"message\":" + json::quote(message) + "}";
}

Response error_response(int status, const std::string& code, const std::string& message) {
    return {status, error_body(code, message)};
}

}  // namespace

bool Endpoint::add(CapabilitySpec spec) {
    if (spec.name.empty() || !spec.handler) {
        return false;
    }
    if (spec.kind != "sensor" && spec.kind != "action" && spec.kind != "stream") {
        return false;
    }
    if (!is_json_object(spec.parameters_json) || !is_json_object(spec.returns_json) ||
        !is_json_object(spec.safety_json)) {
        return false;
    }
    for (const auto& existing : capabilities_) {
        if (existing.name == spec.name) {
            return false;
        }
    }
    capabilities_.push_back(std::move(spec));
    return true;
}

bool Endpoint::name_taken(const std::string& name) const {
    for (const auto& c : health_checks_) {
        if (c.name == name) return true;
    }
    for (const auto& c : self_tests_) {
        if (c.name == name) return true;
    }
    return false;
}

bool Endpoint::add_health_check(std::string name, HealthCheck check) {
    if (name.empty() || !check || name_taken(name)) {
        return false;
    }
    health_checks_.push_back({std::move(name), std::move(check)});
    return true;
}

bool Endpoint::add_self_test(std::string name, HealthCheck check) {
    if (name.empty() || !check || name_taken(name)) {
        return false;
    }
    self_tests_.push_back({std::move(name), std::move(check)});
    return true;
}

bool Endpoint::add_diagnostic(std::string name, DiagnosticProvider provider) {
    if (name.empty() || !provider) {
        return false;
    }
    for (const auto& d : diagnostics_) {
        if (d.first == name) return false;
    }
    diagnostics_.emplace_back(std::move(name), std::move(provider));
    return true;
}

std::string Endpoint::report(const std::vector<const NamedCheck*>& checks,
                             std::string* status_out) const {
    std::string states;
    std::string issues;
    bool any_failed = false;
    bool any_warning = false;

    for (const NamedCheck* named : checks) {
        CheckResult result = named->check();
        const char* state = "ok";
        const char* severity = nullptr;
        const char* default_code = nullptr;
        switch (result.state) {
            case CheckResult::State::Ok: break;
            case CheckResult::State::Skipped: state = "skipped"; break;
            case CheckResult::State::Warning:
                state = "warning"; severity = "warning"; default_code = "CHECK_WARNING";
                any_warning = true;
                break;
            case CheckResult::State::Failed:
                state = "failed"; severity = "error"; default_code = "CHECK_FAILED";
                any_failed = true;
                break;
        }
        if (!states.empty()) states += ',';
        states += json::quote(named->name) + ":\"" + state + "\"";

        if (severity != nullptr) {
            if (!issues.empty()) issues += ',';
            issues += "{\"code\":" +
                      json::quote(result.code.empty() ? default_code : result.code) +
                      ",\"severity\":\"" + severity + "\",\"check\":" +
                      json::quote(named->name) + ",\"message\":" +
                      json::quote(result.message.empty()
                                      ? "Check " + named->name + " reported " + state
                                      : result.message) +
                      "}";
        }
    }

    const char* status = any_failed ? "failed" : any_warning ? "degraded" : "ok";
    if (status_out != nullptr) {
        *status_out = status;
    }
    return "{\"device_id\":" + json::quote(id_) + ",\"status\":\"" + status +
           "\",\"checks\":{" + states + "},\"issues\":[" + issues + "]}";
}

std::string Endpoint::status() const {
    std::vector<const NamedCheck*> checks;
    for (const auto& c : health_checks_) checks.push_back(&c);
    return report(checks, nullptr);
}

std::string Endpoint::self_test() const {
    // esp_http_server runs handlers on one task, so self tests never overlap.
    std::vector<const NamedCheck*> checks;
    for (const auto& c : health_checks_) checks.push_back(&c);
    for (const auto& c : self_tests_) checks.push_back(&c);
    last_self_test_ = report(checks, nullptr);
    return last_self_test_;
}

std::string Endpoint::diagnostics() const {
    std::vector<const NamedCheck*> checks;
    for (const auto& c : health_checks_) checks.push_back(&c);
    std::string status;
    report(checks, &status);

    std::string info;
    for (const auto& d : diagnostics_) {
        std::string value = d.second();
        json::Value parsed;
        std::string error;
        if (!json::parse(value, parsed, error)) {
            value = "{\"error\":" + json::quote("Provider returned invalid JSON: " + error) + "}";
        }
        if (!info.empty()) info += ',';
        info += json::quote(d.first) + ":" + value;
    }

    double uptime_s = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - started_).count();
    return "{\"device_id\":" + json::quote(id_) + ",\"status\":\"" + status +
           "\",\"uptime_s\":" + json::number(uptime_s) +
           ",\"capability_count\":" + std::to_string(capabilities_.size()) +
           ",\"last_self_test\":" + (last_self_test_.empty() ? "null" : last_self_test_) +
           ",\"info\":{" + info + "}}";
}

std::string Endpoint::manifest() const {
    std::string out = "{\"spec_version\":\"0.1\",\"device\":{";
    out += "\"id\":" + json::quote(id_);
    out += ",\"type\":" + json::quote(type_);
    out += ",\"name\":" + json::quote(name_);
    out += ",\"metadata\":{}},\"capabilities\":[";
    for (size_t i = 0; i < capabilities_.size(); ++i) {
        const auto& cap = capabilities_[i];
        if (i > 0) {
            out += ',';
        }
        out += "{\"name\":" + json::quote(cap.name);
        out += ",\"description\":" + json::quote(cap.description);
        out += ",\"kind\":" + json::quote(cap.kind);
        out += ",\"parameters\":" + cap.parameters_json;
        out += ",\"returns\":" + cap.returns_json;
        out += ",\"safety\":" + cap.safety_json + "}";
    }
    out += "]}";
    return out;
}

Response Endpoint::handle(const std::string& method,
                          const std::string& target,
                          const std::string& body) const {
    std::string path = target.substr(0, target.find('?'));

    // Unknown paths and wrong methods both answer 404, matching the Python
    // reference server.
    if (method == "GET" && path == "/health") {
        return {200, "{\"status\":\"ok\",\"device_id\":" + json::quote(id_) + "}"};
    }
    if (method == "GET" && path == "/manifest") {
        return {200, manifest()};
    }
    if (method == "GET" && path == "/status") {
        return {200, status()};
    }
    if (method == "GET" && path == "/diagnostics") {
        return {200, diagnostics()};
    }
    if (method == "POST" && path == "/self_test") {
        return {200, self_test()};
    }
    if (method == "POST" && path == "/invoke") {
        return invoke(body);
    }
    return {404, "{\"error\":\"not_found\"}"};
}

Response Endpoint::invoke(const std::string& body) const {
    json::Value request;
    std::string parse_error;
    if (!json::parse(body, request, parse_error)) {
        return error_response(400, "InvalidRequest", "Malformed JSON: " + parse_error);
    }
    if (!request.is_object()) {
        return error_response(400, "InvalidRequest", "Request body must be a JSON object");
    }

    const json::Value* capability = request.find("capability");
    if (capability == nullptr || !capability->is_string()) {
        return error_response(400, "InvalidRequest", "Missing string field: capability");
    }

    static const json::Value kNoArguments = [] {
        json::Value empty;
        empty.type = json::Value::Type::Object;
        return empty;
    }();
    const json::Value* arguments = request.find("arguments");
    if (arguments == nullptr || arguments->is_null()) {
        arguments = &kNoArguments;
    } else if (!arguments->is_object()) {
        return error_response(400, "InvalidRequest", "Field arguments must be an object");
    }

    for (const auto& cap : capabilities_) {
        if (cap.name != capability->string) {
            continue;
        }
        InvokeResult result = cap.handler(*arguments);
        switch (result.status) {
            case InvokeResult::Status::Ok:
                return {200,
                        "{\"ok\":true,\"capability\":" + json::quote(cap.name) +
                            ",\"result\":" + result.result_json + "}"};
            case InvokeResult::Status::InvalidArguments:
                return error_response(400, "InvalidArguments", result.message);
            case InvokeResult::Status::Failed:
                return error_response(500, "DeviceError", result.message);
        }
    }
    return error_response(404, "UnknownCapability",
                          "Unknown capability: " + capability->string);
}

}  // namespace dc_device::esp32
