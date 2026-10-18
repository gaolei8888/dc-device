#include "dc_device_selfcheck.hpp"

#include "dc_device_endpoint.hpp"
#include "dc_device_json.hpp"

namespace dc_device::esp32 {

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::NotRun: return "not_run";
        case Verdict::Ok: return "ok";
        case Verdict::Failed: return "failed";
        case Verdict::Unverified: return "unverified";
        case Verdict::NotFound: return "not_found";
    }
    return "not_run";
}

const char* class_name(PeripheralClass c) {
    switch (c) {
        case PeripheralClass::Bus: return "bus";
        case PeripheralClass::Signal: return "signal";
        case PeripheralClass::NoSignal: return "no_signal";
    }
    return "bus";
}

bool SelfCheck::add(Detector detector) {
    if (detector.name.empty() || !detector.run) return false;
    for (const auto& d : detectors_) {
        if (d.name == detector.name) return false;
    }
    detectors_.push_back(std::move(detector));
    findings_.emplace_back();
    return true;
}

void SelfCheck::run(Mode mode) {
    for (size_t i = 0; i < detectors_.size(); ++i) {
        Finding f = detectors_[i].run(mode);
        // A Quick run re-checks wiring that is already known. It keeps how that
        // wiring was found; only a Full run replaces it.
        if (mode == Mode::Quick && f.verdict != Verdict::NotFound) {
            if (f.wiring.empty()) f.wiring = findings_[i].wiring;
            if (f.source.empty()) f.source = findings_[i].source;
        }
        findings_[i] = std::move(f);
    }
    ran_ = true;
    last_mode_ = mode;
}

std::string SelfCheck::report_json() const {
    std::string out = std::string("{\"ran\":") + (ran_ ? "true" : "false");
    out += std::string(",\"mode\":\"") + (last_mode_ == Mode::Full ? "full" : "quick") + "\"";
    out += ",\"peripherals\":[";
    for (size_t i = 0; i < detectors_.size(); ++i) {
        if (i) out += ",";
        const Finding& f = findings_[i];
        out += "{\"name\":" + json::quote(detectors_[i].name);
        out += std::string(",\"class\":\"") + class_name(detectors_[i].klass) + "\"";
        out += std::string(",\"verdict\":\"") + verdict_name(f.verdict) + "\"";
        out += ",\"evidence\":" + json::quote(f.evidence);
        out += ",\"wiring\":" + json::quote(f.wiring);
        out += ",\"source\":" + json::quote(f.source) + "}";
    }
    out += "]}";
    return out;
}

static CheckResult aggregate(const SelfCheck& check) {
    std::string failed, missing;
    for (size_t i = 0; i < check.detectors().size(); ++i) {
        const Finding& f = check.findings()[i];
        const std::string& name = check.detectors()[i].name;
        std::string item = name + (f.evidence.empty() ? "" : " (" + f.evidence + ")");
        if (f.verdict == Verdict::Failed) {
            failed += (failed.empty() ? "" : "; ") + item;
        } else if (f.verdict == Verdict::NotFound) {
            missing += (missing.empty() ? "" : "; ") + item;
        }
    }
    if (!failed.empty()) return CheckResult::failed("PERIPHERAL_FAILED", failed);
    if (!missing.empty()) return CheckResult::warning("PERIPHERAL_NOT_FOUND", missing);
    return CheckResult::ok();
}


bool attach_self_check(Endpoint& endpoint, SelfCheck& check) {
    bool ok = endpoint.add_health_check("peripherals", [&check] {
        check.run(Mode::Quick);
        return aggregate(check);
    });
    ok = endpoint.add_self_test("peripheral_discovery", [&check] {
        check.run(Mode::Full);
        return aggregate(check);
    }) && ok;
    ok = endpoint.add_diagnostic("peripherals", [&check] { return check.report_json(); }) && ok;
    return ok;
}

}  // namespace dc_device::esp32
