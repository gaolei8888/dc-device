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
