#include "dc_device_wifi_manager.hpp"

#ifdef ESP_PLATFORM

#include <atomic>
#include <cstring>
#include <memory>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

namespace dc_device::esp32 {

namespace {

const char* TAG = "dc-device-wifi";
const char* kNamespace = "dcsetup";

constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kFailed = BIT1;
constexpr EventBits_t kAssociated = BIT2;

constexpr uint32_t kSetupAttemptMs = 18000;  // the protocol promises an answer within 20 s
constexpr uint32_t kRetryAttemptMs = 15000;
constexpr uint32_t kBackoffStartMs = 5000;
constexpr uint32_t kBackoffMaxMs = 60000;

struct Job {
    enum class Kind { Start, TryNew, Forget, Lost } kind = Kind::Lost;
    std::string ssid;
    std::string password;
    std::function<void(const std::string&)> progress;
    std::function<void(const WifiOutcome&)> done;
    std::shared_ptr<std::atomic<bool>> forget_result;
};

struct Creds {
    std::string ssid;
    std::string password;
    bool empty() const { return ssid.empty(); }
};

struct Shared {
    WifiManager::Callbacks callbacks;
    EventGroupHandle_t events = nullptr;
    QueueHandle_t queue = nullptr;
    std::atomic<WifiState> state{WifiState::Unconfigured};
    std::atomic<int> last_reason{0};
    std::atomic<bool> cancel{false};  // forget() asks a running attempt to stop
    std::atomic<uint32_t> ip_addr{0};  // IPv4, network byte order, 0 = none
    Creds stored;    // what is in NVS (or the dev network); worker only
    bool from_nvs = false;
    bool started = false;
};

Shared g;

bool load_creds(Creds& out) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    char ssid[33] = {};
    char pass[65] = {};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);
    bool ok = nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK;
    if (ok && nvs_get_str(h, "pass", pass, &pass_len) != ESP_OK) pass[0] = '\0';
    nvs_close(h);
    if (!ok || ssid[0] == '\0') return false;
    out.ssid = ssid;
    out.password = pass;
    return true;
}

bool save_creds(const Creds& c) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", c.ssid.c_str()) == ESP_OK &&
              nvs_set_str(h, "pass", c.password.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool erase_creds() {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return true;  // nothing stored
    bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

std::string format_ip() {
    esp_ip4_addr_t a;
    a.addr = g.ip_addr;
    if (a.addr == 0) return {};
    char buf[16];
    esp_ip4addr_ntoa(&a, buf, sizeof(buf));
    return buf;
}

void event_handler(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(g.events, kAssociated);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
        int reason = d != nullptr ? d->reason : 0;
        // We asked for this one (a new attempt, or forgetting the network): not a failure.
        if (reason == WIFI_REASON_ASSOC_LEAVE) return;
        g.last_reason = reason;
        ESP_LOGI(TAG, "disconnected, reason %d", reason);
        xEventGroupClearBits(g.events, kAssociated);
        xEventGroupSetBits(g.events, kFailed);
        if (g.state == WifiState::Connected) {
            // Lost a working connection: the worker retries with a delay.
            g.state = WifiState::Offline;
            auto* job = new Job();
            job->kind = Job::Kind::Lost;
            if (xQueueSend(g.queue, &job, 0) != pdTRUE) delete job;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* d = static_cast<ip_event_got_ip_t*>(data);
        g.ip_addr = d->ip_info.ip.addr;
        xEventGroupSetBits(g.events, kGotIp);
    }
}

// Why a join failed, from the disconnect reason the driver reported.
WifiOutcome failure_for(int reason, bool associated) {
    WifiOutcome o;
    o.ok = false;
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            o.code = "wifi_not_found";
            o.message = "No network with that name was seen.";
            return o;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            o.code = "wifi_auth_failed";
            o.message = "The network rejected the password or the security mode.";
            return o;
        default:
            break;
    }
    if (associated) {
        o.code = "wifi_no_address";
        o.message = "Joined the network but got no address.";
    } else {
        o.code = "wifi_timeout";
        o.message = "The network did not answer in time.";
    }
    return o;
}

// One attempt to join. Does not touch NVS. `progress` may be empty.
WifiOutcome attempt(const Creds& c, uint32_t timeout_ms, const std::function<void(const std::string&)>& progress) {
    xEventGroupClearBits(g.events, kGotIp | kFailed | kAssociated);
    g.last_reason = 0;
    g.ip_addr = 0;

    wifi_config_t cfg{};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), c.ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), c.password.c_str(), sizeof(cfg.sta.password) - 1);
    // An open network has an empty password and must be allowed; otherwise accept
    // whatever the access point offers and let it reject a wrong password.
    cfg.sta.threshold.authmode = c.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WEP;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    esp_wifi_disconnect();
    xEventGroupClearBits(g.events, kGotIp | kFailed | kAssociated);
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        WifiOutcome o;
        o.code = "internal";
        o.message = "The Wi-Fi driver rejected the settings.";
        return o;
    }
    if (progress) progress("connecting");
    esp_wifi_connect();

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    bool told_address = false;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) break;
        // Wake every 100 ms so a forget_wifi does not wait for the whole attempt.
        TickType_t slice = deadline - now < pdMS_TO_TICKS(100) ? deadline - now : pdMS_TO_TICKS(100);
        EventBits_t bits = xEventGroupWaitBits(g.events, kGotIp | kFailed | kAssociated, pdFALSE, pdFALSE, slice);
        if (g.cancel.load()) {
            esp_wifi_disconnect();
            WifiOutcome o;
            o.code = "internal";
            o.message = "Cancelled.";
            return o;
        }
        if (bits & kGotIp) {
            WifiOutcome o;
            o.ok = true;
            o.ip = format_ip();
            return o;
        }
        if (bits & kFailed) {
            // The driver gave up on this association (it retries only if asked).
            WifiOutcome o = failure_for(g.last_reason, told_address || (bits & kAssociated) != 0);
            // Wrong password and unknown network are final. Anything else can be
            // the access point still holding the session of a device that just
            // reset, so ask again while there is time left.
            bool final = o.code == "wifi_auth_failed" || o.code == "wifi_not_found";
            if (final || xTaskGetTickCount() + pdMS_TO_TICKS(4000) >= deadline) return o;
            vTaskDelay(pdMS_TO_TICKS(1000));
            xEventGroupClearBits(g.events, kGotIp | kFailed | kAssociated);
            told_address = false;
            esp_wifi_connect();
            continue;
        }
        if ((bits & kAssociated) && !told_address) {
            told_address = true;
            if (progress) progress("getting_address");
            // Stop waking on the association bit; wait for the address or failure.
            xEventGroupClearBits(g.events, kAssociated);
        }
    }
    esp_wifi_disconnect();
    return failure_for(g.last_reason, told_address);
}

void worker(void*) {
    uint32_t backoff = kBackoffStartMs;
    bool immediate = false;  // the first attempt after boot does not wait
    for (;;) {
        bool want = !g.stored.empty() && g.state != WifiState::Connected;
        Job* job = nullptr;
        BaseType_t got = xQueueReceive(g.queue, &job, want ? (immediate ? 0 : pdMS_TO_TICKS(backoff)) : portMAX_DELAY);
        if (got != pdTRUE) immediate = false;
        if (got != pdTRUE) {
            // Time to retry the stored network.
            g.state = WifiState::Connecting;
            WifiOutcome o = attempt(g.stored, kRetryAttemptMs, {});
            if (o.ok) {
                g.state = WifiState::Connected;
                backoff = kBackoffStartMs;
                if (g.callbacks.on_up) g.callbacks.on_up();
            } else {
                g.state = WifiState::Offline;
                backoff = backoff * 2 > kBackoffMaxMs ? kBackoffMaxMs : backoff * 2;
            }
            continue;
        }
        std::unique_ptr<Job> owned(job);
        switch (owned->kind) {
            case Job::Kind::Start:
                immediate = true;
                break;
            case Job::Kind::Lost:
                // Retry soon rather than after the previous long delay.
                backoff = kBackoffStartMs;
                break;
            case Job::Kind::Forget: {
                g.cancel.store(false);
                esp_wifi_disconnect();
                bool ok = erase_creds();
                if (ok) {
                    g.stored = Creds{};
                    g.from_nvs = false;
                    g.state = WifiState::Unconfigured;
                    if (g.callbacks.on_forgotten) g.callbacks.on_forgotten();
                }
                owned->forget_result->store(ok);
                xEventGroupSetBits(g.events, BIT3);  // answer ready
                break;
            }
            case Job::Kind::TryNew: {
                Creds candidate{owned->ssid, owned->password};
                WifiState before = g.state;
                g.state = WifiState::Connecting;
                WifiOutcome o = attempt(candidate, kSetupAttemptMs, owned->progress);
                if (o.ok) {
                    if (!save_creds(candidate)) {
                        // Joined, but cannot keep it: say so rather than pretend.
                        o.ok = false;
                        o.code = "internal";
                        o.message = "Joined the network but could not store the settings.";
                    }
                }
                if (o.ok) {
                    g.stored = candidate;
                    g.from_nvs = true;
                    g.state = WifiState::Connected;
                    backoff = kBackoffStartMs;
                    if (g.callbacks.on_up) g.callbacks.on_up();
                } else {
                    // Put things back: the old network, if there was one, is retried.
                    g.state = g.stored.empty() ? WifiState::Unconfigured
                                               : (before == WifiState::Connected ? WifiState::Offline : before);
                    if (g.stored.empty()) esp_wifi_disconnect();
                    backoff = kBackoffStartMs;
                }
                owned->done(o);
                break;
            }
        }
    }
}

}  // namespace

bool WifiManager::begin(const std::string& dev_ssid, const std::string& dev_password, Callbacks callbacks) {
    if (g.started) return true;
    g.callbacks = std::move(callbacks);
    g.events = xEventGroupCreate();
    g.queue = xQueueCreate(4, sizeof(Job*));
    if (g.events == nullptr || g.queue == nullptr) return false;

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, nullptr) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, nullptr) != ESP_OK) {
        return false;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) return false;
    // Modem sleep delays inbound packets; a device that serves HTTP must stay awake.
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (load_creds(g.stored)) {
        g.from_nvs = true;
    } else if (!dev_ssid.empty()) {
        g.stored = Creds{dev_ssid, dev_password};  // development only; not saved
    }
    g.state = g.stored.empty() ? WifiState::Unconfigured : WifiState::Connecting;
    g.started = true;
    if (xTaskCreate(worker, "dc_wifi", 6144, nullptr, 5, nullptr) != pdPASS) return false;
    if (!g.stored.empty()) {
        // First attempt right away.
        auto* job = new Job();
        job->kind = Job::Kind::Start;
        if (xQueueSend(g.queue, &job, 0) != pdTRUE) delete job;
    }
    ESP_LOGI(TAG, "Wi-Fi %s", g.stored.empty() ? "not configured; waiting for USB setup" : "settings found");
    return true;
}

WifiState WifiManager::state() const { return g.state; }
std::string WifiManager::ip() const { return g.state == WifiState::Connected ? format_ip() : std::string(); }

void WifiManager::try_network(const std::string& ssid, const std::string& password,
                              std::function<void(const std::string&)> progress,
                              std::function<void(const WifiOutcome&)> done) {
    auto* job = new Job();
    job->kind = Job::Kind::TryNew;
    job->ssid = ssid;
    job->password = password;
    job->progress = std::move(progress);
    job->done = done;
    if (!g.started || xQueueSend(g.queue, &job, 0) != pdTRUE) {
        delete job;
        WifiOutcome o;
        o.code = "internal";
        o.message = "The Wi-Fi task is not available.";
        done(o);
    }
}

bool WifiManager::forget() {
    if (!g.started) return true;
    auto* job = new Job();
    job->kind = Job::Kind::Forget;
    job->forget_result = std::make_shared<std::atomic<bool>>(false);
    auto result = job->forget_result;
    xEventGroupClearBits(g.events, BIT3);
    g.cancel.store(true);
    if (xQueueSend(g.queue, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
        g.cancel.store(false);
        delete job;
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(g.events, BIT3, pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
    return (bits & BIT3) != 0 && result->load();
}

SetupBackend WifiManager::backend(uint16_t http_port) {
    SetupBackend b;
    b.state = [this] { return state(); };
    b.ip = [this] { return ip(); };
    b.port = http_port;
    b.start_wifi = [this](const std::string& ssid, const std::string& password,
                          std::function<void(const std::string&)> progress,
                          std::function<void(const WifiOutcome&)> done) {
        try_network(ssid, password, std::move(progress), std::move(done));
    };
    b.forget_wifi = [this] { return forget(); };
    return b;
}

}  // namespace dc_device::esp32

#else  // host build: the manager needs the ESP-IDF Wi-Fi driver

namespace dc_device::esp32 {
bool WifiManager::begin(const std::string&, const std::string&, Callbacks) { return false; }
WifiState WifiManager::state() const { return WifiState::Unconfigured; }
std::string WifiManager::ip() const { return {}; }
void WifiManager::try_network(const std::string&, const std::string&, std::function<void(const std::string&)>,
                              std::function<void(const WifiOutcome&)> done) {
    WifiOutcome o;
    o.code = "internal";
    done(o);
}
bool WifiManager::forget() { return false; }
SetupBackend WifiManager::backend(uint16_t) { return {}; }
}  // namespace dc_device::esp32

#endif
