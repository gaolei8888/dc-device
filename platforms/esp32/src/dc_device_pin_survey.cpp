#ifdef ESP_PLATFORM

#include "dc_device_pin_survey.hpp"

#include <memory>
#include <string>

#include "dc_device_chip.hpp"
#include "dc_device_pins.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace dc_device::esp32 {

std::vector<int> pins_held_high() {
    esp_log_level_set("gpio", ESP_LOG_WARN);
    std::vector<int> out;
    for (int pin : safe_scan_pins(read_chip_facts().model)) {
        if (pin_claimed(pin)) continue;
        gpio_num_t g = static_cast<gpio_num_t>(pin);
        gpio_reset_pin(g);
        gpio_set_direction(g, GPIO_MODE_INPUT);
        gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY);
        esp_rom_delay_us(2000);
        if (gpio_get_level(g) == 1) out.push_back(pin);
        gpio_reset_pin(g);
    }
    return out;
}

std::vector<int> pins_floating() {
    esp_log_level_set("gpio", ESP_LOG_WARN);
    std::vector<int> out;
    for (int pin : safe_scan_pins(read_chip_facts().model)) {
        if (pin_claimed(pin)) continue;
        gpio_num_t g = static_cast<gpio_num_t>(pin);
        gpio_reset_pin(g);
        gpio_set_direction(g, GPIO_MODE_INPUT);
        gpio_set_pull_mode(g, GPIO_PULLUP_ONLY);
        esp_rom_delay_us(2000);
        int up = gpio_get_level(g);
        gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY);
        esp_rom_delay_us(2000);
        int down = gpio_get_level(g);
        gpio_reset_pin(g);
        if (up == 1 && down == 0) out.push_back(pin);
    }
    return out;
}

Detector make_pin_survey_detector() {
    auto last = std::make_shared<Finding>();
    last->name = "pin_survey";
    last->klass = PeripheralClass::Signal;
    last->verdict = Verdict::Unverified;
    last->evidence = "not run yet; run the self test";

    Detector d;
    d.name = "pin_survey";
    d.klass = PeripheralClass::Signal;
    d.run_many = [last](Mode mode) {
        if (mode == Mode::Full) {
            std::string high, low, floating, in_use;
            for (int pin : safe_scan_pins(read_chip_facts().model)) {
                if (pin_claimed(pin)) {
                    in_use += (in_use.empty() ? "" : " ") + std::to_string(pin);
                    continue;
                }
                gpio_num_t g = static_cast<gpio_num_t>(pin);
                gpio_reset_pin(g);
                gpio_set_direction(g, GPIO_MODE_INPUT);
                gpio_set_pull_mode(g, GPIO_PULLUP_ONLY);
                esp_rom_delay_us(2000);
                int up = gpio_get_level(g);
                gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY);
                esp_rom_delay_us(2000);
                int down = gpio_get_level(g);
                gpio_reset_pin(g);
                std::string p = std::to_string(pin);
                if (up == 1 && down == 0) {
                    floating += (floating.empty() ? "" : " ") + p;
                } else if (up == 1 && down == 1) {
                    high += (high.empty() ? "" : " ") + p;
                } else if (up == 0 && down == 0) {
                    low += (low.empty() ? "" : " ") + p;
                } else {
                    high += (high.empty() ? "" : " ") + p + "?";  // went the wrong way: noise
                }
            }
            last->verdict = Verdict::Ok;
            last->evidence = "held high: [" + high + "]; held low: [" + low + "]; follow the pull (nothing holds them): [" +
                             floating + "]" +
                             (in_use.empty() ? "" : "; skipped, in use by a bus: [" + in_use + "]");
        }
        return std::vector<Finding>{*last};
    };
    return d;
}

}  // namespace dc_device::esp32

#endif
