#include "dc_device_esp32.hpp"

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#endif

namespace dc_device::esp32 {

bool GpioOutput::begin() {
#ifdef ESP_PLATFORM
    gpio_config_t config{};
    config.pin_bit_mask = (1ULL << pin_);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config) == ESP_OK;
#else
    return true;
#endif
}

bool GpioOutput::write(bool high) {
#ifdef ESP_PLATFORM
    return gpio_set_level(static_cast<gpio_num_t>(pin_), high ? 1 : 0) == ESP_OK;
#else
    (void)high;
    return true;
#endif
}

bool PwmOutput::begin(int frequency_hz, int resolution_bits) {
#ifdef ESP_PLATFORM
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = LEDC_TIMER_0;
    timer.duty_resolution = static_cast<ledc_timer_bit_t>(resolution_bits);
    timer.freq_hz = frequency_hz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        return false;
    }

    ledc_channel_config_t channel{};
    channel.gpio_num = pin_;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = static_cast<ledc_channel_t>(channel_);
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 0;
    channel.hpoint = 0;
    return ledc_channel_config(&channel) == ESP_OK;
#else
    (void)frequency_hz;
    (void)resolution_bits;
    return true;
#endif
}

bool PwmOutput::write_duty(float duty_0_to_1) {
    if (duty_0_to_1 < 0.0f || duty_0_to_1 > 1.0f) {
        return false;
    }
#ifdef ESP_PLATFORM
    constexpr uint32_t max_duty = 65535;
    uint32_t duty = static_cast<uint32_t>(duty_0_to_1 * max_duty);
    if (ledc_set_duty(
            LEDC_LOW_SPEED_MODE,
            static_cast<ledc_channel_t>(channel_),
            duty) != ESP_OK) {
        return false;
    }
    return ledc_update_duty(
               LEDC_LOW_SPEED_MODE,
               static_cast<ledc_channel_t>(channel_)) == ESP_OK;
#else
    return true;
#endif
}

bool Servo::begin() {
    return pwm_.begin(50, 16);
}

bool Servo::write_angle(float angle_deg) {
    if (angle_deg < -90.0f || angle_deg > 90.0f) {
        return false;
    }

    // Typical hobby servo: 0.5ms..2.5ms pulse at 20ms period.
    float pulse_ms = 1.5f + (angle_deg / 90.0f);
    float duty = pulse_ms / 20.0f;
    return pwm_.write_duty(duty);
}

}  // namespace dc_device::esp32
