#!/bin/sh
# Builds and runs the host-side Base64 / screen.draw_rect tests (no ESP-IDF needed).
set -e
cd "$(dirname "$0")/../.."
OUT="${TMPDIR:-/tmp}/dc_device_screen_rect_tests"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
    -fsanitize=address,undefined \
    -Iplatforms/esp32/include \
    tests/esp32/test_screen_rect.cpp \
    platforms/esp32/src/dc_device_json.cpp \
    -o "$OUT"
"$OUT"
