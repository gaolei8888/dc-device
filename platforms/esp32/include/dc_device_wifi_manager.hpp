#pragma once

#include <functional>
#include <string>

#include "dc_device_setup.hpp"

namespace dc_device::esp32 {

// Keeps the device on Wi-Fi without ever blocking the caller, and lets the USB setup
// protocol (dc_device_setup.hpp) change the network.
//
// States: unconfigured (nothing stored), connecting, connected, offline (stored
// credentials but the network cannot be joined; retried at 5 s doubling to 60 s,
// never by rebooting). Credentials are written to NVS only after a connection
// succeeded, so a typo does not destroy a working configuration. They are never
// logged.
//
// A network compiled into the firmware (`dev_ssid`) is only a development
// convenience: it is used when nothing is stored and is never saved.
class WifiManager {
public:
    struct Callbacks {
        // The device has an address (first time, and again after a drop). Must be
        // safe to call repeatedly.
        std::function<void()> on_up;
        // Credentials were erased: stop serving HTTP.
        std::function<void()> on_forgotten;
    };

    // The default network interface and event loop must exist (esp_netif_init and
    // esp_event_loop_create_default). Returns false if Wi-Fi could not start.
    bool begin(const std::string& dev_ssid, const std::string& dev_password, Callbacks callbacks);

    WifiState state() const;
    std::string ip() const;

    // The two operations behind the setup protocol; same contract as SetupBackend.
    void try_network(const std::string& ssid, const std::string& password,
                     std::function<void(const std::string&)> progress,
                     std::function<void(const WifiOutcome&)> done);
    bool forget();

    // The setup protocol's view of this manager.
    SetupBackend backend(uint16_t http_port);
};

}  // namespace dc_device::esp32
