#pragma once

#include <functional>
#include <string>
#include <vector>

namespace dc_device::esp32 {

class Endpoint;

// Detector framework from docs/self-check-v0.1.md. A detector looks for one
// peripheral and says what it could observe. It never has to be told how the
// peripheral is wired.

// How the device can know about the peripheral (design doc, "Three kinds").
enum class PeripheralClass {
    Bus,       // answers on a bus with an identity
    Signal,    // shows an electrical effect
    NoSignal,  // nothing comes back: only a person can confirm
};

enum class Verdict {
    NotRun,
    Ok,          // observed working
    Failed,      // expected and not found, or found and misbehaving
    Unverified,  // present as far as known, but not observable
    NotFound,    // looked for and absent
};

// Quick: only wiring that is already known or cached. Cheap and read-only; the
// mode used from /status. Full: may also search for the wiring. Runs only from
// an explicit /self_test.
enum class Mode { Quick, Full };

struct Finding {
    Verdict verdict = Verdict::NotRun;
    std::string evidence;  // what was seen, e.g. "chip id 0x2642 at 0x30"
    std::string wiring;    // what was found, e.g. "SDA 4, SCL 5"; empty if unknown
    std::string source;    // "known_map" or "scan"; empty if none
};

struct Detector {
    std::string name;  // unique, non-empty
    PeripheralClass klass = PeripheralClass::Bus;
    std::function<Finding(Mode)> run;
};

const char* verdict_name(Verdict v);
const char* class_name(PeripheralClass c);

class SelfCheck {
public:
    // Returns false (and registers nothing) for an empty or duplicate name or a
    // missing run function.
    bool add(Detector detector);

    // Runs every detector once and remembers the result.
    void run(Mode mode);

    // {"ran":bool,"mode":"quick"|"full","peripherals":[{name,class,verdict,
    // evidence,wiring,source}]}. Before the first run every verdict is not_run.
    std::string report_json() const;

    size_t size() const { return detectors_.size(); }
    const std::vector<Finding>& findings() const { return findings_; }
    const std::vector<Detector>& detectors() const { return detectors_; }
    bool ran() const { return ran_; }

private:
    std::vector<Detector> detectors_;
    std::vector<Finding> findings_;
    bool ran_ = false;
    Mode last_mode_ = Mode::Quick;
};

// Registers three things on the endpoint:
//   health check "peripherals"  - runs Quick; cheap
//   self test    "peripheral_discovery" - runs Full (names are shared by checks and tests)
//   diagnostic   "peripherals"  - the last report, with evidence and wiring
// `check` must outlive the endpoint.
//
// Aggregate state: any Failed -> failed; else any NotFound -> warning; else ok.
// Unverified does not degrade the device (a device with an LED would otherwise
// never be healthy); the per-peripheral verdict is in the diagnostic. The
// contract has no state for it yet (docs/self-check-v0.1.md, open items).
bool attach_self_check(Endpoint& endpoint, SelfCheck& check);

}  // namespace dc_device::esp32
