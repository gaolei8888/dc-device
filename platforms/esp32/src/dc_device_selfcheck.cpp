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
    if (detector.name.empty()) return false;
    if (static_cast<bool>(detector.run) == static_cast<bool>(detector.run_many)) return false;
    for (const auto& d : detectors_) {
        if (d.name == detector.name) return false;
    }
    detectors_.push_back(std::move(detector));
    findings_.emplace_back(1);  // one not_run placeholder
    seen_ok_.emplace_back();
    return true;
}

std::vector<Entry> SelfCheck::entries() const {
    std::vector<Entry> out;
    for (size_t i = 0; i < detectors_.size(); ++i) {
        for (const Finding& f : findings_[i]) {
            Entry e;
            e.name = f.name.empty() ? detectors_[i].name : f.name;
            e.klass = f.name.empty() ? detectors_[i].klass : f.klass;
            e.finding = f;
            e.required = detectors_[i].required;
            out.push_back(std::move(e));
        }
    }
    return out;
}

void SelfCheck::run(Mode mode) {
    for (size_t i = 0; i < detectors_.size(); ++i) {
        std::vector<Finding> fresh;
        if (detectors_[i].run) {
            fresh.push_back(detectors_[i].run(mode));
        } else {
            fresh = detectors_[i].run_many(mode);
        }
        // A Quick run re-checks wiring that is already known. It keeps how that
        // wiring was found; only a Full run replaces it.
        if (mode == Mode::Quick) {
            for (Finding& f : fresh) {
                if (f.verdict == Verdict::NotFound) continue;
                for (const Finding& old : findings_[i]) {
                    if (old.name != f.name) continue;
                    if (f.wiring.empty()) f.wiring = old.wiring;
                    if (f.source.empty()) f.source = old.source;
                }
            }
        }
        // Something that answered earlier and is silent now is a fault, not an
        // absent part. Only Quick runs re-check known wiring, so only they judge.
        if (mode == Mode::Quick) {
            for (Finding& f : fresh) {
                if (f.verdict == Verdict::NotFound && seen_ok_[i].count(f.name) != 0) {
                    f.verdict = Verdict::Failed;
                    f.evidence = "answered earlier, now silent (" + f.evidence + ")";
                }
            }
            for (const auto& seen : seen_ok_[i]) {
                bool present = false;
                for (const Finding& f : fresh) present = present || f.name == seen.first;
                if (present) continue;
                Finding gone = seen.second;
                gone.verdict = Verdict::Failed;
                gone.evidence = "answered earlier, now silent";
                fresh.push_back(std::move(gone));
            }
        }
        for (const Finding& f : fresh) {
            if (f.verdict == Verdict::Ok) seen_ok_[i][f.name] = f;
        }
        findings_[i] = std::move(fresh);
        // A multi-peripheral detector that found nothing still leaves a row, so
        // the report shows that it ran.
        if (findings_[i].empty()) {
            Finding none;
            none.verdict = Verdict::NotFound;
            none.evidence = "nothing found";
            findings_[i].push_back(std::move(none));
        }
    }
    ran_ = true;
    last_mode_ = mode;
}

std::string SelfCheck::report_json() const {
    std::string out = std::string("{\"ran\":") + (ran_ ? "true" : "false");
    out += std::string(",\"mode\":\"") + (last_mode_ == Mode::Full ? "full" : "quick") + "\"";
    out += ",\"peripherals\":[";
    bool first = true;
    for (const Entry& e : entries()) {
        if (!first) out += ",";
        first = false;
        const Finding& f = e.finding;
        out += "{\"name\":" + json::quote(e.name);
        out += std::string(",\"class\":\"") + class_name(e.klass) + "\"";
        out += std::string(",\"verdict\":\"") + verdict_name(f.verdict) + "\"";
        out += ",\"evidence\":" + json::quote(f.evidence);
        out += ",\"wiring\":" + json::quote(f.wiring);
        out += ",\"source\":" + json::quote(f.source);
        if (!e.required) out += ",\"optional\":true";
        if (!f.confirm_capability.empty()) {
            out += ",\"confirm\":{\"capability\":" + json::quote(f.confirm_capability) +
                   ",\"question\":" + json::quote(f.confirm_question) + "}";
        }
        out += "}";
    }
    out += "]}";
    return out;
}

Detector make_human_confirmed_detector(std::string name, std::string wiring, std::string source,
                                       std::string capability, std::string question) {
    Detector d;
    d.name = std::move(name);
    d.klass = PeripheralClass::NoSignal;
    d.run = [wiring = std::move(wiring), source = std::move(source), capability = std::move(capability),
             question = std::move(question)](Mode) {
        Finding f;
        f.verdict = Verdict::Unverified;
        f.evidence = "the device cannot observe this; a person has to check it";
        f.wiring = wiring;
        f.source = source;
        f.confirm_capability = capability;
        f.confirm_question = question;
        return f;
    };
    return d;
}

const Finding* SelfCheck::finding(const std::string& name) const {
    for (size_t i = 0; i < detectors_.size(); ++i) {
        for (const Finding& f : findings_[i]) {
            if ((f.name.empty() ? detectors_[i].name : f.name) == name) return &f;
        }
    }
    return nullptr;
}

Detector make_heard_by_detector(std::string name, std::string wiring, std::string source,
                                std::string capability, std::string question,
                                const SelfCheck& check, std::string listener) {
    Detector d = make_human_confirmed_detector(name, wiring, source, capability, question);
    Detector human = d;
    d.klass = PeripheralClass::Signal;
    d.run = [human, &check, listener = std::move(listener)](Mode mode) {
        const Finding* heard = check.finding(listener);
        if (heard == nullptr || heard->verdict != Verdict::Ok || !heard->heard_stimulus) {
            return human.run(mode);
        }
        Finding f;
        f.verdict = Verdict::Ok;
        f.klass = PeripheralClass::Signal;
        f.evidence = "the " + listener + " heard the tone this board played";
        f.wiring = human.run(mode).wiring;
        f.source = human.run(mode).source;
        return f;
    };
    return d;
}

static CheckResult aggregate(const SelfCheck& check) {
    std::string failed, missing;
    for (const Entry& e : check.entries()) {
        const Finding& f = e.finding;
        std::string item = e.name + (f.evidence.empty() ? "" : " (" + f.evidence + ")");
        if (f.verdict == Verdict::Failed) {
            failed += (failed.empty() ? "" : "; ") + item;
        } else if (f.verdict == Verdict::NotFound && e.required) {
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
