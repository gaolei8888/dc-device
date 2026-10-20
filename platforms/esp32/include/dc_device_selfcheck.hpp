#pragma once

#include <functional>
#include <map>
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
    // Set only by a detector that reports several peripherals (run_many): each
    // finding names itself and its class. Empty name = the detector's own name.
    std::string name = "";
    PeripheralClass klass = PeripheralClass::Bus;
    // For a peripheral only a person can check: the capability that makes it do
    // something visible or audible, and the question to put to the person after it.
    // The device cannot know the answer; remembering it is the host's job.
    std::string confirm_capability = "";
    std::string confirm_question = "";
};

struct Detector {
    std::string name;  // unique, non-empty
    PeripheralClass klass = PeripheralClass::Bus;
    std::function<Finding(Mode)> run = nullptr;  // one peripheral ...
    // ... or several (a bus scan finds a variable number). Exactly one of the two.
    std::function<std::vector<Finding>(Mode)> run_many = nullptr;
    // false = the part may legitimately be absent (a microphone on a board that has
    // none): not_found is reported but does not degrade /status. The device cannot
    // know which parts a given board should have; the host that owns that
    // knowledge compares the report with it. Always last, so brace init still works.
    bool required = true;
};

// One row of the report: a peripheral and what was observed.
struct Entry {
    std::string name;
    PeripheralClass klass = PeripheralClass::Bus;
    Finding finding;
    bool required = true;
};

const char* verdict_name(Verdict v);
const char* class_name(PeripheralClass c);

class SelfCheck {
public:
    // Returns false (and registers nothing) for an empty or duplicate name, or
    // unless exactly one of run and run_many is set.
    bool add(Detector detector);

    // Runs every detector once and remembers the result.
    void run(Mode mode);

    // {"ran":bool,"mode":"quick"|"full","peripherals":[{name,class,verdict,
    // evidence,wiring,source[,"optional":true]}]}. Before the first run every verdict is not_run.
    std::string report_json() const;

    size_t size() const { return detectors_.size(); }
    // Every peripheral reported so far, in detector order. Before the first run:
    // one not_run row per detector.
    std::vector<Entry> entries() const;
    bool ran() const { return ran_; }

private:
    std::vector<Detector> detectors_;
    std::vector<std::vector<Finding>> findings_;  // per detector
    // Per detector: peripherals seen working since boot, with their last good row.
    std::vector<std::map<std::string, Finding>> seen_ok_;
    bool ran_ = false;
    Mode last_mode_ = Mode::Quick;
};

// A peripheral nothing on the board can observe (a speaker, a screen): always
// `unverified`, with the capability that exercises it and the question to ask a
// person. `wiring` and `source` say where it is and where that came from; "known_map"
// means taken from a table, not measured.
Detector make_human_confirmed_detector(std::string name, std::string wiring, std::string source,
                                       std::string capability, std::string question);

// Registers three things on the endpoint:
//   health check "peripherals"  - runs Quick; cheap
//   self test    "peripheral_discovery" - runs Full (names are shared by checks and tests)
//   diagnostic   "peripherals"  - the last report, with evidence and wiring
// `check` must outlive the endpoint.
//
// Aggregate state: any Failed -> failed; else any NotFound of a required detector ->
// warning; else ok. A part that answered earlier in this boot and no longer does is
// Failed (Quick runs), whether or not it is required.
// Unverified does not degrade the device (a device with an LED would otherwise
// never be healthy); the per-peripheral verdict is in the diagnostic. The
// contract has no state for it yet (docs/self-check-v0.1.md, open items).
bool attach_self_check(Endpoint& endpoint, SelfCheck& check);

}  // namespace dc_device::esp32
