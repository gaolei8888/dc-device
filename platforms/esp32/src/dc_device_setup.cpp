#include "dc_device_setup.hpp"

#include <cmath>
#include <utility>

#include "dc_device_json.hpp"

namespace dc_device::esp32 {

namespace {

const char kPrefix[] = "DCSETUP ";
constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;

bool has_prefix(const std::string& line) {
    return line.compare(0, kPrefixLength, kPrefix) == 0;
}

std::string line_for(const std::string& json) { return std::string(kPrefix) + json; }

std::string state_field(WifiState state) { return std::string("\"state\":\"") + wifi_state_name(state) + "\""; }

}  // namespace

const char* wifi_state_name(WifiState state) {
    switch (state) {
        case WifiState::Unconfigured: return "unconfigured";
        case WifiState::Connecting: return "connecting";
        case WifiState::Connected: return "connected";
        case WifiState::Offline: return "offline";
    }
    return "unconfigured";
}

void SetupProtocol::send(const std::string& json) { write_(line_for(json)); }

void SetupProtocol::reply_error(const std::string& id_json, const char* code, const std::string& message,
                                const std::string& extra) {
    send("{\"v\":1,\"id\":" + id_json + ",\"ok\":false,\"error\":{\"code\":" + json::quote(code) +
         ",\"message\":" + json::quote(message) + "}" + extra + "}");
}

void SetupProtocol::feed(const char* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        char c = data[i];
        if (c == '\n') {
            if (!discarding_) handle_line(line_);
            line_.clear();
            discarding_ = false;
            continue;
        }
        if (discarding_) continue;
        line_.push_back(c);
        // The limit counts the line break's \r too, which is ignored: a line of
        // exactly the limit plus a \r is still rejected, which is fine.
        if (line_.size() > kSetupMaxLine) {
            // Only a protocol line gets an answer; a long log line is just noise.
            if (has_prefix(line_)) reply_error("null", "line_too_long", "The line is longer than 512 bytes.");
            line_.clear();
            discarding_ = true;
        }
    }
}

void SetupProtocol::handle_line(const std::string& raw) {
    std::string line = raw;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || !has_prefix(line)) return;  // log output, not for us

    json::Value request;
    std::string error;
    if (!json::parse(line.substr(kPrefixLength), request, error) || !request.is_object()) {
        reply_error("null", "bad_request", "The line is not a JSON object.");
        return;
    }

    // The id is echoed in every answer, so find it first. Without a usable one the
    // answer has "id":null.
    std::string id = "null";
    const json::Value* id_value = request.find("id");
    if (id_value != nullptr && id_value->is_number() && std::floor(id_value->number) == id_value->number) {
        id = json::number(id_value->number);
    }
    if (id == "null") {
        reply_error(id, "bad_request", "\"id\" must be an integer.");
        return;
    }

    const json::Value* version = request.find("v");
    if (version == nullptr || !version->is_number()) {
        reply_error(id, "bad_request", "\"v\" must be an integer.");
        return;
    }
    if (version->number != 1) {
        reply_error(id, "unsupported_version", "Only protocol version 1 is supported.", ",\"supported\":[1]");
        return;
    }

    const json::Value* cmd = request.find("cmd");
    if (cmd == nullptr || !cmd->is_string()) {
        reply_error(id, "bad_request", "\"cmd\" must be a string.");
        return;
    }

    if (cmd->string == "hello") {
        hello(id);
    } else if (cmd->string == "wifi") {
        const json::Value* ssid = request.find("ssid");
        const json::Value* password = request.find("password");
        if (ssid == nullptr || !ssid->is_string() || password == nullptr || !password->is_string()) {
            reply_error(id, "bad_request", "\"ssid\" and \"password\" must be strings.");
            return;
        }
        wifi(id, ssid->string, password->string);
    } else if (cmd->string == "forget_wifi") {
        forget(id);
    } else if (cmd->string == "parts") {
        parts(id);
    } else if (cmd->string == "declare") {
        declare(id, request);
    } else {
        reply_error(id, "unknown_command", "Unknown command.");
    }
}

void SetupProtocol::hello(const std::string& id) {
    WifiState state = backend_.state ? backend_.state() : WifiState::Unconfigured;
    std::string out = "{\"v\":1,\"id\":" + id + ",\"ok\":true,\"setup_protocol\":1";
    out += ",\"device_id\":" + json::quote(info_.device_id);
    out += ",\"type\":" + json::quote(info_.type);
    out += ",\"model\":" + json::quote(info_.model);
    out += ",\"firmware_version\":" + json::quote(info_.firmware_version);
    out += ",\"serial\":" + json::quote(info_.serial);
    if (!info_.profile_id.empty()) {
        out += ",\"profile\":{\"id\":" + json::quote(info_.profile_id) +
               ",\"version\":" + json::quote(info_.profile_version) + "}";
    }
    out += "," + state_field(state);
    if (state == WifiState::Connected) {
        std::string ip = backend_.ip ? backend_.ip() : "";
        out += ",\"port\":" + std::to_string(backend_.port);
        if (!ip.empty()) out += ",\"ip\":" + json::quote(ip);
    }
    out += "}";
    send(out);
}

void SetupProtocol::wifi(const std::string& id, const std::string& ssid, const std::string& password) {
    if (ssid.empty() || ssid.size() > 32) {
        reply_error(id, "invalid_ssid", "The network name must be 1 to 32 bytes.");
        return;
    }
    if (!password.empty() && (password.size() < 8 || password.size() > 63)) {
        reply_error(id, "invalid_password", "The password must be empty or 8 to 63 characters.");
        return;
    }
    if (!backend_.start_wifi) {
        reply_error(id, "internal", "This device cannot join a network.");
        return;
    }
    bool expected = false;
    if (!busy_->compare_exchange_strong(expected, true)) {
        reply_error(id, "busy", "A previous command is still running.");
        return;
    }

    // Copies, not references: the answer comes later, possibly from another task.
    auto write = write_;
    auto busy = busy_;
    uint16_t port = backend_.port;
    auto progress = [write, id](const std::string& stage) {
        write(line_for("{\"v\":1,\"id\":" + id + ",\"event\":\"progress\",\"stage\":" + json::quote(stage) + "}"));
    };
    auto done = [write, busy, id, port](const WifiOutcome& outcome) {
        std::string json;
        if (outcome.ok) {
            json = "{\"v\":1,\"id\":" + id + ",\"ok\":true,\"state\":\"connected\",\"ip\":" +
                   json::quote(outcome.ip) + ",\"port\":" + std::to_string(port) + "}";
        } else {
            json = "{\"v\":1,\"id\":" + id + ",\"ok\":false,\"error\":{\"code\":" +
                   json::quote(outcome.code.empty() ? "internal" : outcome.code) +
                   ",\"message\":" + json::quote(outcome.message) + "}}";
        }
        busy->store(false);
        write(line_for(json));
    };
    backend_.start_wifi(ssid, password, progress, done);
}

void SetupProtocol::forget(const std::string& id) {
    if (busy_->load()) {
        reply_error(id, "busy", "A previous command is still running.");
        return;
    }
    if (!backend_.forget_wifi || !backend_.forget_wifi()) {
        reply_error(id, "internal", "Could not erase the stored network.");
        return;
    }
    send("{\"v\":1,\"id\":" + id + ",\"ok\":true,\"state\":\"unconfigured\"}");
}

void SetupProtocol::parts(const std::string& id) {
    std::string list;
    if (backend_.parts) {
        for (const auto& part : backend_.parts()) {
            if (!list.empty()) list += ',';
            list += "{\"name\":" + json::quote(part.first) + ",\"declared\":" + (part.second ? "true" : "false") + "}";
        }
    }
    send("{\"v\":1,\"id\":" + id + ",\"ok\":true,\"parts\":[" + list + "]}");
}

void SetupProtocol::declare(const std::string& id, const json::Value& request) {
    const json::Value* part = request.find("part");
    const json::Value* present = request.find("present");
    if (part == nullptr || !part->is_string() || present == nullptr || !present->is_bool()) {
        reply_error(id, "bad_request", "\"part\" must be a string and \"present\" a boolean.");
        return;
    }
    PartResult result = backend_.declare_part ? backend_.declare_part(part->string, present->boolean)
                                              : PartResult::UnknownPart;
    if (result == PartResult::UnknownPart) {
        reply_error(id, "unknown_part", "This device has no part with that name to declare.");
        return;
    }
    if (result == PartResult::Failed) {
        reply_error(id, "internal", "Could not store the answer.");
        return;
    }
    send("{\"v\":1,\"id\":" + id + ",\"ok\":true,\"part\":" + json::quote(part->string) +
         ",\"present\":" + (present->boolean ? "true" : "false") + "}");
}

}  // namespace dc_device::esp32
