#pragma once

#include <cstdint>

namespace dc_device::esp32 {

class GpioOutput {
public:
    explicit GpioOutput(int pin) : pin_(pin) {}
    bool begin();
    bool write(bool high);
    int pin() const { return pin_; }

private:
    int pin_;
};

class PwmOutput {
public:
    PwmOutput(int pin, int channel) : pin_(pin), channel_(channel) {}
    bool begin(int frequency_hz = 50, int resolution_bits = 16);
    bool write_duty(float duty_0_to_1);

private:
    int pin_;
    int channel_;
};

class Servo {
public:
    Servo(int pin, int channel) : pwm_(pin, channel) {}
    bool begin();
    bool write_angle(float angle_deg);

private:
    PwmOutput pwm_;
};

}  // namespace dc_device::esp32
