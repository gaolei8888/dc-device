#include "dc_device_screen_blocks.hpp"

namespace dc_device::esp32 {

RectCheck check_rect(int x, int y, int width, int height) {
    if (x < 0 || y < 0 || width < 1 || height < 1) return RectCheck::OutOfRange;
    if (x > kScreenWidth - width || y > kScreenHeight - height) return RectCheck::OutOfRange;
    return RectCheck::Ok;
}

BlockCheck check_block(int x, int y, int width, int height, size_t byte_count) {
    if (check_rect(x, y, width, height) != RectCheck::Ok) return BlockCheck::OutOfRange;
    size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
    if (byte_count != expected || byte_count > kMaxBlockBytes) return BlockCheck::BadBlockSize;
    return BlockCheck::Ok;
}

}  // namespace dc_device::esp32
