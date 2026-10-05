#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dc_device {

enum class CapabilityKind {
    Sensor,
    Action,
    Stream
};

struct Capability {
    std::string name;
    std::string description;
    CapabilityKind kind;
};

class Device {
public:
    Device(std::string id, std::string type, std::string name)
        : id_(std::move(id)), type_(std::move(type)), name_(std::move(name)) {}

    void register_capability(const Capability& capability) {
        capabilities_.push_back(capability);
    }

    const std::vector<Capability>& capabilities() const {
        return capabilities_;
    }

private:
    std::string id_;
    std::string type_;
    std::string name_;
    std::vector<Capability> capabilities_;
};

}  // namespace dc_device
