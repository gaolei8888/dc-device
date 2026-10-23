#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dc_device_selfcheck.hpp"

namespace dc_device::esp32 {

// Parts nothing on the board can observe (a screen, a speaker without a microphone, later a
// motor) are not declared in /manifest on the strength of a table. A person who sees the
// part says so, over the USB cable (usb-setup-protocol-v0.1.md, `declare`), and the
// answer is kept here across restarts. See docs/self-check-v0.1.md, "Declaring
// capabilities".

struct PartStorage {
    std::function<std::map<std::string, bool>()> load;               // stored flags, empty if none
    std::function<bool(const std::string& part, bool present)> save;  // persists one flag
};

class DeclaredParts {
public:
    enum class SetResult { Ok, UnknownPart, StorageFailed };

    // `known`: the parts this firmware can have declared, in the order list() reports them.
    // Stored flags for any other name are ignored.
    DeclaredParts(std::vector<std::string> known, PartStorage storage);

    bool declared(const std::string& part) const;  // false if unknown or not set
    SetResult set(const std::string& part, bool present);
    std::vector<std::pair<std::string, bool>> list() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> known_;
    std::map<std::string, bool> flags_;
    PartStorage storage_;
};

// Availability for CapabilitySpec::available: the capability that exercises `part` is declared
// when the self-check observed the part working (verdict ok), or a person declared it and
// nothing says otherwise (verdict not_found or failed beats the declaration).
// `check` and `parts` must outlive the returned function.
std::function<bool()> available_when_confirmed(const SelfCheck& check, std::string part,
                                               const DeclaredParts& parts);

}  // namespace dc_device::esp32
