#include "dc_device_setup_console.hpp"

#ifdef ESP_PLATFORM

#include <cstdio>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace dc_device::esp32 {

namespace {

const char* TAG = "dc-device-setup";

SemaphoreHandle_t out_lock() {
    static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    return lock;
}

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
constexpr uart_port_t kPort = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);

void reader(void* raw) {
    auto* protocol = static_cast<SetupProtocol*>(raw);
    uint8_t buffer[128];
    for (;;) {
        int n = uart_read_bytes(kPort, buffer, sizeof(buffer), pdMS_TO_TICKS(100));
        if (n > 0) protocol->feed(reinterpret_cast<const char*>(buffer), static_cast<size_t>(n));
    }
}
#endif

}  // namespace

void write_setup_line(const std::string& line) {
    SemaphoreHandle_t lock = out_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    // The leading line break ends any log line that was half written.
    std::fputc('\n', stdout);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    xSemaphoreGive(lock);
}

bool start_setup_console(SetupProtocol& protocol) {
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    // Output keeps going through stdout; only input needs the driver.
    if (!uart_is_driver_installed(kPort) && uart_driver_install(kPort, 512, 0, 0, nullptr, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Could not read the console port; USB setup is unavailable");
        return false;
    }
    if (xTaskCreate(reader, "dc_setup", 4096, &protocol, 4, nullptr) != pdPASS) return false;
    ESP_LOGI(TAG, "USB setup ready on UART %d", static_cast<int>(kPort));
    return true;
#else
    (void)protocol;
    ESP_LOGW(TAG, "The console is not a UART; USB setup is unavailable on this build");
    return false;
#endif
}

}  // namespace dc_device::esp32

#else

namespace dc_device::esp32 {
void write_setup_line(const std::string&) {}
bool start_setup_console(SetupProtocol&) { return false; }
}  // namespace dc_device::esp32

#endif
