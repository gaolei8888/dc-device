#pragma once

#include <cstddef>

namespace dc_device::esp32 {

// The checks behind screen.draw_rect and screen.write_block (docs/superpowers/specs/
// 2026-10-22-camera-capture-and-screen-block-contract.md). Plain code, so they run in host tests;
// the SPI part is in the camera_board example.

constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 320;
constexpr size_t kMaxBlockBytes = 7680;  // RGB565, two bytes per pixel: 240 x 16 rows

enum class RectCheck { Ok, OutOfRange };
enum class BlockCheck { Ok, OutOfRange, BadBlockSize };

// The rectangle must have at least one pixel and lie inside the screen.
RectCheck check_rect(int x, int y, int width, int height);

// Range first, then size: exactly width * height * 2 bytes and at most kMaxBlockBytes.
BlockCheck check_block(int x, int y, int width, int height, size_t byte_count);

}  // namespace dc_device::esp32
