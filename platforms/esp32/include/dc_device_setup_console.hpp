#pragma once

#include "dc_device_setup.hpp"

namespace dc_device::esp32 {

// Reads the console serial port in its own task and feeds the setup protocol.
// Answers go back to the same port, each on a line of its own, mixed with the
// firmware's log output (the protocol expects that). Nothing here blocks on
// Wi-Fi: the port is read and answered in every network state.
//
// Supports the UART console (CONFIG_ESP_CONSOLE_UART_DEFAULT, which is what a board
// with a USB-to-serial chip uses). On a board whose console is the chip's native USB
// (USB Serial/JTAG) it logs that setup is unavailable and returns false.
//
// `protocol` must outlive the task (use a static).
bool start_setup_console(SetupProtocol& protocol);

// The writer to hand to SetupProtocol: one line, with a line break before and after,
// serialised so two tasks do not interleave a protocol line.
void write_setup_line(const std::string& line);

}  // namespace dc_device::esp32
