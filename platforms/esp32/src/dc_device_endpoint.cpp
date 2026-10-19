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

// Profile ids: ^[a-z0-9][a-z0-9._-]*$
bool valid_profile_id(const std::string& id) {
    if (id.empty()) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '-'))) return false;
    }
    return true;
}

// Profile versions: ^[0-9]+\.[0-9]+\.[0-9]+$
bool valid_profile_version(const std::string& version) {
    int parts = 0;
    size_t digits = 0;
    for (char c : version) {
        if (c >= '0' && c <= '9') {
            ++digits;
        } else if (c == '.' && digits > 0 && parts < 2) {
            ++parts;
            digits = 0;
        } else {
            return false;
        }
    }
    return parts == 2 && digits > 0;
}

bool values_equal(const json::Value& a, const json::Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case json::Value::Type::Null: return true;
        case json::Value::Type::Bool: return a.boolean == b.boolean;
        case json::Value::Type::Number: return a.number == b.number;
        case json::Value::Type::String: return a.string == b.string;
        default: break;
    }
    if (a.keys != b.keys || a.items.size() != b.items.size()) return false;
    for (size_t i = 0; i < a.items.size(); ++i) {
        if (!values_equal(a.items[i], b.items[i])) return false;
    }
    return true;
}

bool matches_type(const json::Value& value, const std::string& type) {
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number() && value.number == static_cast<long long>(value.number);
    if (type == "boolean") return value.is_bool();
    if (type == "string") return value.is_string();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    return true;  // unknown types are not enforced, as in the Python SDK
}

// Checks arguments against the manifest schema subset the Python SDK enforces:
// required, type, enum, minimum, maximum. Returns an empty string when valid.
std::string validate_arguments(const std::string& schema_json, const json::Value& arguments) {
    json::Value schema;
    std::string error;
    if (!json::parse(schema_json, schema, error)) return {};
    if (const json::Value* required = schema.find("required")) {
        for (const auto& name : required->items) {
            if (name.is_string() && arguments.find(name.string) == nullptr) {
                return "Missing required argument: " + name.string;
            }
        }
    }
    const json::Value* properties = schema.find("properties");
    if (properties == nullptr) return {};
    for (size_t i = 0; i < arguments.keys.size(); ++i) {
        const std::string& name = arguments.keys[i];
        const json::Value& value = arguments.items[i];
        const json::Value* rule = properties->find(name);
        if (rule == nullptr) continue;
        const json::Value* type = rule->find("type");
        if (type != nullptr && type->is_string() && !matches_type(value, type->string)) {
            return "Argument " + name + " must be of type " + type->string;
        }
        if (const json::Value* allowed = rule->find("enum")) {
            bool found = false;
            for (const auto& option : allowed->items) found = found || values_equal(option, value);
            if (!found) return "Argument " + name + " must be one of the allowed values";
        }
        if (value.is_number()) {
            const json::Value* min = rule->find("minimum");
            if (min != nullptr && min->is_number() && value.number < min->number) {
                return "Argument " + name + " must be >= " + json::number(min->number);
            }
            const json::Value* max = rule->find("maximum");
            if (max != nullptr && max->is_number() && value.number > max->number) {
                return "Argument " + name + " must be <= " + json::number(max->number);
            }
        }
    }
    return {};
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
    // docs/safety-semantics-v0.1.md: max_rate_hz is a minimum interval between
    // accepted invocations; emergency capabilities are exempt.
    json::Value safety;
    std::string safety_error;
    json::parse(spec.safety_json, safety, safety_error);
    RateState rate;
    bool emergency = false;
    if (const json::Value* flag = safety.find("emergency")) {
        if (!flag->is_bool()) return false;
        emergency = flag->boolean;
    }
    if (const json::Value* motion = safety.find("motion")) {
        if (!motion->is_bool()) return false;
        rate.motion = motion->boolean;
    }
    if (const json::Value* confirm = safety.find("requires_confirmation")) {
        // An emergency capability that needs confirmation is not an emergency stop.
        if (!confirm->is_bool() || (emergency && confirm->boolean)) return false;
    }
    if (const json::Value* ms = safety.find("max_duration_ms")) {
        // Whole milliseconds > 0; an emergency capability cannot be time limited.
        if (!ms->is_number() || !(ms->number >= 1) ||
            ms->number != static_cast<double>(static_cast<long long>(ms->number)) || emergency) {
            return false;
        }
        rate.max_duration_s = ms->number / 1000.0;
    }
    if (const json::Value* hz = safety.find("max_rate_hz")) {
        if (!hz->is_number() || !(hz->number > 0)) return false;
        if (!emergency) rate.min_interval_s = 1.0 / hz->number;
    }
    capabilities_.push_back(std::move(spec));
    rates_.push_back(rate);
    return true;
}

double Endpoint::now_s() const {
    return clock_ ? clock_()
                  : std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

bool Endpoint::set_safe_state(const std::string& capability, bool safe_on_boot) {
    for (const auto& cap : capabilities_) {
        if (cap.name != capability) continue;
        json::Value safety;
        json::Value parameters;
        std::string error;
        json::parse(cap.safety_json, safety, error);
        json::parse(cap.parameters_json, parameters, error);
        const json::Value* emergency = safety.find("emergency");
        const json::Value* required = parameters.find("required");
        if (emergency == nullptr || !emergency->is_bool() || !emergency->boolean) return false;
        if (required != nullptr && !required->items.empty()) return false;
        safe_state_ = capability;
        safe_on_boot_ = safe_on_boot;
        return true;
    }
    return false;
}

bool Endpoint::validate_safety() const {
    if (!safe_state_.empty()) return true;
    for (const auto& r : rates_) {
        if (r.max_duration_s > 0 || r.motion) return false;
    }
    return true;
}

bool Endpoint::tick() {
    if (safe_state_.empty()) return false;
    double now = now_s();
    bool expired = false;
    for (const auto& r : rates_) {
        expired = expired || (r.armed && r.deadline_s <= now);
    }
    if (!expired) return false;
    for (auto& r : rates_) r.armed = false;
    static const json::Value kNoArguments = [] {
        json::Value empty;
        empty.type = json::Value::Type::Object;
        return empty;
    }();
    for (const auto& cap : capabilities_) {
        if (cap.name == safe_state_) {
            cap.handler(kNoArguments);
            break;
        }
    }
    return true;
}

bool Endpoint::time_limited_running() const {
    for (const auto& r : rates_) {
        if (r.armed) return true;
    }
    return false;
}

bool Endpoint::set_identity(Identity identity) {
    bool has_id = !identity.profile_id.empty();
    bool has_version = !identity.profile_version.empty();
    if (has_id != has_version) {
        return false;
    }
    if (has_id && (!valid_profile_id(identity.profile_id) ||
                   !valid_profile_version(identity.profile_version))) {
        return false;
    }
    identity_ = std::move(identity);
    return true;
}

std::string Endpoint::identity() const {
    std::string out = "{\"spec_version\":\"0.1\",\"device_id\":" + json::quote(id_) +
                      ",\"type\":" + json::quote(type_) + ",\"name\":" + json::quote(name_);
    auto optional = [&out](const char* key, const std::string& value) {
        if (!value.empty()) {
            out += std::string(",\"") + key + "\":" + json::quote(value);
        }
    };
    optional("manufacturer", identity_.manufacturer);
    optional("model", identity_.model);
    optional("hardware_revision", identity_.hardware_revision);
    optional("firmware_version", identity_.firmware_version);
    optional("serial", identity_.serial);
    if (!identity_.profile_id.empty()) {
        out += ",\"profile\":{\"id\":" + json::quote(identity_.profile_id) +
               ",\"version\":" + json::quote(identity_.profile_version) + "}";
    }
    out += "}";
    return out;
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
                             std::string* status_out, size_t run_first_from) const {
    std::string states;
    std::string issues;
    bool any_failed = false;
    bool any_warning = false;

    // Entries from `run_first_from` on (the self tests) run before the ones
    // ahead of them (the health checks), so a health check sees what a self test
    // just found. The report keeps its original order.
    std::vector<CheckResult> results(checks.size());
    for (size_t i = run_first_from; i < checks.size(); ++i) results[i] = checks[i]->check();
    for (size_t i = 0; i < run_first_from && i < checks.size(); ++i) results[i] = checks[i]->check();

    for (size_t idx = 0; idx < checks.size(); ++idx) {
        const NamedCheck* named = checks[idx];
        const CheckResult& result = results[idx];
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
    size_t first_self_test = checks.size();
    for (const auto& c : self_tests_) checks.push_back(&c);
    last_self_test_ = report(checks, nullptr, first_self_test);
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
    out += "]";
    if (!safe_state_.empty()) {
        out += ",\"safety_kernel\":{\"safe_state\":" + json::quote(safe_state_) +
               ",\"safe_on_boot\":" + (safe_on_boot_ ? "true" : "false") + "}";
    }
    out += "}";
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
    if (method == "GET" && path == "/identity") {
        return {200, identity()};
    }
    if (method == "GET" && path == "/status") {
        return {200, status()};
    }
    if (method == "GET" && path == "/diagnostics") {
        return {200, diagnostics()};
    }
    if (method == "POST" && path == "/self_test") {
        if (time_limited_running()) {
            return error_response(409, "Busy",
                                  "A time-limited capability is running; stop it before a self test");
        }
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

    for (size_t index = 0; index < capabilities_.size(); ++index) {
        const auto& cap = capabilities_[index];
        if (cap.name != capability->string) {
            continue;
        }
        std::string invalid = validate_arguments(cap.parameters_json, *arguments);
        if (!invalid.empty()) {
            return error_response(400, "InvalidArguments", invalid);
        }
        RateState& rate = rates_[index];
        if (rate.min_interval_s > 0) {
            double now = now_s();
            if (rate.has_last && now - rate.last_s < rate.min_interval_s) {
                long wait_ms = static_cast<long>((rate.min_interval_s - (now - rate.last_s)) * 1000) + 1;
                return error_response(429, "RateLimited",
                                      cap.name + " is rate limited; retry in " +
                                          std::to_string(wait_ms) + " ms");
            }
            rate.has_last = true;
            rate.last_s = now;
        }
        InvokeResult result = cap.handler(*arguments);
        if (result.status == InvokeResult::Status::Ok) {
            if (cap.name == safe_state_) {
                for (auto& r : rates_) r.armed = false;
            }
            if (rate.max_duration_s > 0) {
                rate.armed = true;
                rate.deadline_s = now_s() + rate.max_duration_s;
            }
        }
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
