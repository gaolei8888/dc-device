#include "dc_device_declared_parts.hpp"

#include <algorithm>

namespace dc_device::esp32 {

DeclaredParts::DeclaredParts(std::vector<std::string> known, PartStorage storage)
    : known_(std::move(known)), storage_(std::move(storage)) {
    if (!storage_.load) return;
    for (const auto& entry : storage_.load()) {
        if (std::find(known_.begin(), known_.end(), entry.first) != known_.end()) {
            flags_[entry.first] = entry.second;
        }
    }
}

bool DeclaredParts::declared(const std::string& part) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = flags_.find(part);
    return it != flags_.end() && it->second;
}

DeclaredParts::SetResult DeclaredParts::set(const std::string& part, bool present) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(known_.begin(), known_.end(), part) == known_.end()) return SetResult::UnknownPart;
    if (storage_.save && !storage_.save(part, present)) return SetResult::StorageFailed;
    flags_[part] = present;
    return SetResult::Ok;
}

std::vector<std::pair<std::string, bool>> DeclaredParts::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, bool>> out;
    for (const auto& name : known_) {
        auto it = flags_.find(name);
        out.emplace_back(name, it != flags_.end() && it->second);
    }
    return out;
}

std::function<bool()> available_when_confirmed(const SelfCheck& check, std::string part,
                                               const DeclaredParts& parts) {
    return [&check, &parts, part = std::move(part)] {
        const Finding* finding = check.finding(part);
        Verdict verdict = finding != nullptr ? finding->verdict : Verdict::NotRun;
        if (verdict == Verdict::Ok) return true;
        if (verdict == Verdict::NotFound || verdict == Verdict::Failed) return false;
        return parts.declared(part);
    };
}

}  // namespace dc_device::esp32
