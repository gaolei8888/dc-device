// Camera board: joins Wi-Fi, serves the dc-device HTTP contract and checks its
// own peripherals (docs/self-check-v0.1.md). I2C devices (the camera sensor, and
// any IMU, codec or touch chip) are found by their chip ids; nobody has to say
// which pins they are on. /status re-checks the
// known or cached wiring; POST /self_test may search for it. Nothing here drives
// a data line or takes a frame.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "dc_device_base64.hpp"
#include "dc_device_declared_parts.hpp"
#include "dc_device_i2c_detect.hpp"
#include "dc_device_image_analysis.hpp"
#include "dc_device_parts_nvs.hpp"
#include "dc_device_screen_blocks.hpp"
#include "dc_device_mic_detect.hpp"
#include "dc_device_pin_survey.hpp"
#include <vector>
#include "dc_device_chip.hpp"
#include "dc_device_endpoint.hpp"
#include "dc_device_http_server.hpp"
#include "dc_device_selfcheck.hpp"
#include "dc_device_setup.hpp"
#include "dc_device_setup_console.hpp"
#include "dc_device_wifi_manager.hpp"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "mbedtls/sha256.h"
#include <atomic>
#include <mutex>
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

using dc_device::esp32::CheckResult;
using dc_device::esp32::Endpoint;
using dc_device::esp32::InvokeResult;
namespace json = dc_device::json;


// Pins from the board's original program (the same two boards). Used only by the
// two tests below, which need a person to see or hear the result.
namespace {
std::string g_screen_error;  // first failing step of screen_test()
void note(const char* step, esp_err_t e) {
    if (e != ESP_OK && g_screen_error.empty()) g_screen_error = std::string(step) + ": " + esp_err_to_name(e);
}
constexpr int kSpkBclk = 42, kSpkWs = 39, kSpkDout = 41;
constexpr int kTftSck = 46, kTftMosi = 3, kTftDc = 1, kTftCs = 14;

// A short, quiet tone on the I2S speaker output.
bool beep(int freq_hz, int ms, int amplitude) {
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) return false;
    i2s_std_config_t sc{};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    sc.gpio_cfg.bclk = static_cast<gpio_num_t>(kSpkBclk);
    sc.gpio_cfg.ws = static_cast<gpio_num_t>(kSpkWs);
    sc.gpio_cfg.dout = static_cast<gpio_num_t>(kSpkDout);
    sc.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
        i2s_del_channel(tx);
        return false;
    }
    // Written in chunks: the whole tone in one buffer (35 KB for a second) ran the heap out.
    const int samples = 16000 * ms / 1000;
    std::vector<int16_t> buf(1600);
    size_t written = 0;
    for (int done = 0; done < samples; done += static_cast<int>(buf.size())) {
        int n = std::min<int>(buf.size(), samples - done);
        for (int i = 0; i < n; ++i) {
            buf[i] = static_cast<int16_t>(amplitude * std::sin(2.0 * 3.14159265 * freq_hz * (done + i) / 16000.0));
        }
        i2s_channel_write(tx, buf.data(), n * sizeof(int16_t), &written, 2000);
    }
    std::vector<int16_t> silence(3200, 0);  // let the amplifier settle, no click
    i2s_channel_write(tx, silence.data(), silence.size() * sizeof(int16_t), &written, 1000);
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    for (int p : {kSpkBclk, kSpkWs, kSpkDout}) {
        gpio_reset_pin(static_cast<gpio_num_t>(p));
        gpio_set_direction(static_cast<gpio_num_t>(p), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(p), 0);  // the original program parks them low
    }
    return true;
}

// ST7789, 240x320, written only: nothing can be read back. Fills the screen with
// red, green, blue, then black.
bool tft_cmd(spi_device_handle_t dev, uint8_t cmd, const uint8_t* data = nullptr, size_t n = 0) {
    gpio_set_level(static_cast<gpio_num_t>(kTftDc), 0);
    spi_transaction_t t{};
    t.length = 8;
    t.tx_buffer = &cmd;
    esp_err_t e1 = spi_device_polling_transmit(dev, &t);
    note("command byte", e1);
    if (e1 != ESP_OK) return false;
    if (n > 0) {
        gpio_set_level(static_cast<gpio_num_t>(kTftDc), 1);
        spi_transaction_t d{};
        d.length = n * 8;
        d.tx_buffer = data;
        if (spi_device_polling_transmit(dev, &d) != ESP_OK) return false;
    }
    return true;
}

bool tft_fill(spi_device_handle_t dev, uint16_t color) {
    const uint8_t col[4] = {0, 0, 0, 239}, row[4] = {0, 0, 0x01, 0x3F};
    if (!tft_cmd(dev, 0x2A, col, 4) || !tft_cmd(dev, 0x2B, row, 4)) return false;
    uint8_t* chunk = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_DMA));
    if (chunk == nullptr) return false;
    for (int i = 0; i < 4096; i += 2) {
        chunk[i] = color >> 8;
        chunk[i + 1] = color & 0xFF;
    }
    uint8_t ramwr = 0x2C;
    gpio_set_level(static_cast<gpio_num_t>(kTftDc), 0);
    spi_transaction_t t{};
    t.length = 8;
    t.tx_buffer = &ramwr;
    esp_err_t re = spi_device_polling_transmit(dev, &t);
    note("RAMWR", re);
    bool ok = re == ESP_OK;
    gpio_set_level(static_cast<gpio_num_t>(kTftDc), 1);
    size_t left = 240 * 320 * 2;
    while (ok && left > 0) {
        size_t n = left < 4096 ? left : 4096;
        spi_transaction_t d{};
        d.length = n * 8;
        d.tx_buffer = chunk;
        esp_err_t de = spi_device_polling_transmit(dev, &d);
        note("pixel data", de);
        ok = de == ESP_OK;
        left -= n;
    }
    heap_caps_free(chunk);
    return ok;
}

// The panel stays open between calls once a screen capability has started it, so a picture can
// arrive in pieces (screen.write_block). screen.fill_test closes it first and starts over, as
// it always did, because this panel is known to be unreliable.
spi_device_handle_t g_tft = nullptr;
bool g_tft_bus = false;

void tft_release() {
    if (g_tft != nullptr) spi_bus_remove_device(g_tft);
    g_tft = nullptr;
    if (g_tft_bus) spi_bus_free(SPI2_HOST);
    g_tft_bus = false;
    for (int p : {kTftSck, kTftMosi, kTftDc, kTftCs}) gpio_reset_pin(static_cast<gpio_num_t>(p));
}

// Starts the bus and the panel (reset, sleep out, 16 bits per pixel, display on) unless they
// are already open. On failure it closes whatever it opened and g_screen_error says why.
bool tft_open() {
    if (g_tft != nullptr) return true;
    gpio_config_t dc{};
    dc.pin_bit_mask = 1ULL << kTftDc;
    dc.mode = GPIO_MODE_OUTPUT;
    gpio_config(&dc);
    spi_bus_config_t bus{};
    bus.mosi_io_num = kTftMosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = kTftSck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    g_screen_error.clear();
    esp_err_t be = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    note("spi_bus_initialize", be);
    if (be != ESP_OK) {
        tft_release();
        return false;
    }
    g_tft_bus = true;
    spi_device_interface_config_t dev_cfg{};
    dev_cfg.clock_speed_hz = 20 * 1000 * 1000;
    dev_cfg.mode = 2;  // polarity 1, phase 0, as the original program
    dev_cfg.spics_io_num = kTftCs;
    dev_cfg.queue_size = 1;
    esp_err_t ae = spi_bus_add_device(SPI2_HOST, &dev_cfg, &g_tft);
    note("spi_bus_add_device", ae);
    if (ae != ESP_OK) {
        g_tft = nullptr;
        tft_release();
        return false;
    }
    tft_cmd(g_tft, 0x01);  // software reset
    vTaskDelay(pdMS_TO_TICKS(150));
    tft_cmd(g_tft, 0x11);  // sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t colmod = 0x55;  // 16 bits per pixel
    tft_cmd(g_tft, 0x3A, &colmod, 1);
    const uint8_t madctl = 0x00;
    tft_cmd(g_tft, 0x36, &madctl, 1);
    tft_cmd(g_tft, 0x21);  // inversion on, as most ST7789 panels need
    tft_cmd(g_tft, 0x13);  // normal display mode
    tft_cmd(g_tft, 0x29);  // display on
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!g_screen_error.empty()) {  // a command could not even be sent
        tft_release();
        return false;
    }
    return true;
}

bool screen_test() {
    tft_release();
    if (!tft_open()) return false;
    bool ok = true;
    for (uint16_t color : {uint16_t{0xF800}, uint16_t{0x07E0}, uint16_t{0x001F}, uint16_t{0x0000}}) {
        ok = tft_fill(g_tft, color) && ok;
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    return ok;
}

// Opens the rectangle x..x+w-1, y..y+h-1 for the pixels that follow.
bool tft_window(int x, int y, int w, int h) {
    const uint8_t col[4] = {uint8_t(x >> 8), uint8_t(x), uint8_t((x + w - 1) >> 8), uint8_t(x + w - 1)};
    const uint8_t row[4] = {uint8_t(y >> 8), uint8_t(y), uint8_t((y + h - 1) >> 8), uint8_t(y + h - 1)};
    return tft_cmd(g_tft, 0x2A, col, 4) && tft_cmd(g_tft, 0x2B, row, 4);
}

// Sends `count` pixels' worth of bytes (RGB565, high byte first) to the open window. `source`
// either points at the bytes or, when null, `fill` is repeated. Chunks go through a DMA buffer.
bool tft_pixels(const uint8_t* source, uint16_t fill, size_t byte_count) {
    uint8_t* chunk = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_DMA));
    if (chunk == nullptr) {
        note("dma buffer", ESP_ERR_NO_MEM);
        return false;
    }
    if (source == nullptr) {
        for (int i = 0; i < 4096; i += 2) {
            chunk[i] = fill >> 8;
            chunk[i + 1] = fill & 0xFF;
        }
    }
    uint8_t ramwr = 0x2C;
    gpio_set_level(static_cast<gpio_num_t>(kTftDc), 0);
    spi_transaction_t t{};
    t.length = 8;
    t.tx_buffer = &ramwr;
    esp_err_t re = spi_device_polling_transmit(g_tft, &t);
    note("RAMWR", re);
    bool ok = re == ESP_OK;
    gpio_set_level(static_cast<gpio_num_t>(kTftDc), 1);
    size_t done = 0;
    while (ok && done < byte_count) {
        size_t n = byte_count - done < 4096 ? byte_count - done : 4096;
        if (source != nullptr) memcpy(chunk, source + done, n);
        spi_transaction_t d{};
        d.length = n * 8;
        d.tx_buffer = chunk;
        esp_err_t de = spi_device_polling_transmit(g_tft, &d);
        note("pixel data", de);
        ok = de == ESP_OK;
        done += n;
    }
    heap_caps_free(chunk);
    return ok;
}

bool int_arg(const json::Value& args, const char* key, int& out) {
    const json::Value* v = args.find(key);
    if (v == nullptr || !v->is_number()) return false;
    out = static_cast<int>(v->number);
    return true;
}

InvokeResult screen_draw_rect(const json::Value& args) {
    int x = 0, y = 0, w = 0, h = 0, color = 0;
    if (!int_arg(args, "x", x) || !int_arg(args, "y", y) || !int_arg(args, "width", w) ||
        !int_arg(args, "height", h) || !int_arg(args, "color", color)) {
        return InvokeResult::invalid("x, y, width, height and color are required integers");
    }
    if (color < 0 || color > 65535) return InvokeResult::invalid("color must be 0..65535 (RGB565)");
    if (dc_device::esp32::check_rect(x, y, w, h) != dc_device::esp32::RectCheck::Ok) {
        return InvokeResult::invalid("The rectangle lies outside the 240 x 320 screen", "out_of_range");
    }
    g_screen_error.clear();
    if (!tft_open() || !tft_window(x, y, w, h) || !tft_pixels(nullptr, static_cast<uint16_t>(color), size_t(w) * h * 2)) {
        return InvokeResult::failed("Could not drive the screen (" + g_screen_error + ")", "screen_not_ready");
    }
    return InvokeResult::success("{\"drawn\":true}");
}

InvokeResult screen_write_block(const json::Value& args) {
    int x = 0, y = 0, w = 0, h = 0;
    const json::Value* data = args.find("data_base64");
    if (!int_arg(args, "x", x) || !int_arg(args, "y", y) || !int_arg(args, "width", w) ||
        !int_arg(args, "height", h) || data == nullptr || !data->is_string()) {
        return InvokeResult::invalid("x, y, width, height and data_base64 are required");
    }
    // 7680 bytes of pixels are 10240 base64 characters; refuse anything longer before decoding.
    if (data->string.size() > (dc_device::esp32::kMaxBlockBytes + 2) / 3 * 4) {
        return InvokeResult::invalid("The block is larger than 7680 bytes", "bad_block_size");
    }
    std::vector<uint8_t> bytes;
    if (!dc_device::base64_decode(data->string, bytes)) {
        return InvokeResult::invalid("data_base64 is not valid base64", "bad_base64");
    }
    switch (dc_device::esp32::check_block(x, y, w, h, bytes.size())) {
        case dc_device::esp32::BlockCheck::OutOfRange:
            return InvokeResult::invalid("The block lies outside the 240 x 320 screen", "out_of_range");
        case dc_device::esp32::BlockCheck::BadBlockSize:
            return InvokeResult::invalid("data must be width x height x 2 bytes (RGB565, high byte first), at most 7680",
                                         "bad_block_size");
        case dc_device::esp32::BlockCheck::Ok:
            break;
    }
    g_screen_error.clear();
    if (!tft_open() || !tft_window(x, y, w, h) || !tft_pixels(bytes.data(), 0, bytes.size())) {
        return InvokeResult::failed("Could not drive the screen (" + g_screen_error + ")", "screen_not_ready");
    }
    return InvokeResult::success("{\"written\":" + std::to_string(bytes.size()) + "}");
}

// ---- camera.capture -------------------------------------------------------------------
// The sensor is an OV2640 on a DVP adapter that has its own oscillator, so no clock pin is
// driven (xclk_pin -1). The driver only delivers frames with the frame buffers in PSRAM;
// without it every frame overflowed (EV-VSYNC-OVF).
constexpr size_t kMaxJpegBytes = 100000;
std::atomic<bool> g_camera_busy{false};

camera_config_t camera_config(framesize_t size, int jpeg_quality) {
    camera_config_t c{};
    c.pin_pwdn = -1; c.pin_reset = -1; c.pin_xclk = -1;
    c.pin_sccb_sda = 4; c.pin_sccb_scl = 5;
    c.pin_d0 = 11; c.pin_d1 = 9; c.pin_d2 = 8; c.pin_d3 = 10;
    c.pin_d4 = 12; c.pin_d5 = 18; c.pin_d6 = 17; c.pin_d7 = 16;
    c.pin_vsync = 6; c.pin_href = 7; c.pin_pclk = 13;
    c.xclk_freq_hz = 24000000;
    c.ledc_timer = LEDC_TIMER_0; c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size = size;
    c.jpeg_quality = jpeg_quality;
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;
    c.sccb_i2c_port = -1;
    return c;
}

// Takes one frame with the given settings. The first frames after start are discarded: the
// exposure has not settled and the oldest buffers are stale. On success `jpeg` holds the picture.
InvokeResult camera_take(framesize_t size, int jpeg_quality, std::vector<uint8_t>& jpeg, int& width, int& height) {
    camera_config_t c = camera_config(size, jpeg_quality);
    const esp_err_t e = esp_camera_init(&c);
    if (e == ESP_ERR_NOT_FOUND) return InvokeResult::failed("The camera sensor does not answer", "camera_not_found");
    if (e == ESP_ERR_NO_MEM) return InvokeResult::failed("Not enough memory for the camera frame buffers", "out_of_memory");
    if (e != ESP_OK) return InvokeResult::failed(std::string("The camera driver did not start (") + esp_err_to_name(e) + ")", "camera_init_failed");
    camera_fb_t* fb = nullptr;
    for (int i = 0; i < 4; ++i) {
        if (fb != nullptr) esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
        if (fb == nullptr) break;
    }
    if (fb == nullptr) {
        esp_camera_deinit();
        return InvokeResult::failed("The sensor answers but no frame arrived", "camera_no_frame");
    }
    jpeg.assign(fb->buf, fb->buf + fb->len);
    width = fb->width;
    height = fb->height;
    esp_camera_fb_return(fb);
    esp_camera_deinit();
    return InvokeResult::success();
}

// ---- honest declaration -----------------------------------------------------------------
// camera.capture is declared only after this board took a picture itself and the picture
// carried detail. A sensor that answers on the bus proves nothing about the data lines.
std::mutex g_probe_mu;
bool g_probed = false;
dc_device::esp32::Finding g_probe;  // the last probe, read by /manifest and by the quick check

dc_device::esp32::Finding camera_probe_now() {
    using dc_device::esp32::Verdict;
    dc_device::esp32::Finding f;
    f.name = "camera_image";
    f.klass = dc_device::esp32::PeripheralClass::Signal;
    f.wiring = "SCCB 4/5, D0-D7 11,9,8,10,12,18,17,16, PCLK 13, VSYNC 6, HREF 7, no XCLK pin";
    f.source = "known_map";
    if (g_camera_busy.exchange(true)) {
        std::lock_guard<std::mutex> lock(g_probe_mu);
        return g_probe;  // a capture is running: keep the last answer
    }
    std::vector<uint8_t> jpeg;
    int w = 0, h = 0;
    InvokeResult r = camera_take(FRAMESIZE_QVGA, 20, jpeg, w, h);
    if (r.status != InvokeResult::Status::Ok) {
        f.verdict = r.code == "camera_not_found" ? Verdict::NotFound : Verdict::Failed;
        f.evidence = r.message + " (" + r.code + ")";
    } else {
        // Decode at 1/8 size (40 x 30) and judge the brightness values.
        const size_t out_size = static_cast<size_t>((w + 7) / 8) * ((h + 7) / 8) * 3;
        std::vector<uint8_t> rgb(out_size + 16);
        esp_jpeg_image_cfg_t cfg{};
        cfg.indata = jpeg.data();
        cfg.indata_size = static_cast<uint32_t>(jpeg.size());
        cfg.outbuf = rgb.data();
        cfg.outbuf_size = static_cast<uint32_t>(rgb.size());
        cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
        cfg.out_scale = JPEG_IMAGE_SCALE_1_8;
        esp_jpeg_image_output_t out{};
        if (esp_jpeg_decode(&cfg, &out) != ESP_OK || out.output_len < 3) {
            f.verdict = Verdict::Failed;
            f.evidence = "a frame arrived but it is not a decodable JPEG";
        } else {
            std::vector<uint8_t> gray(out.output_len / 3);
            for (size_t i = 0; i < gray.size(); ++i) {
                gray[i] = static_cast<uint8_t>((rgb[3 * i] * 30 + rgb[3 * i + 1] * 59 + rgb[3 * i + 2] * 11) / 100);
            }
            f.verdict = dc_device::esp32::judge_frame(dc_device::esp32::analyze_gray(gray.data(), gray.size()), f.evidence);
        }
    }
    g_camera_busy = false;
    std::lock_guard<std::mutex> lock(g_probe_mu);
    g_probed = true;
    g_probe = f;
    return f;
}

dc_device::esp32::Finding camera_last_probe() {
    std::lock_guard<std::mutex> lock(g_probe_mu);
    if (g_probed) return g_probe;
    dc_device::esp32::Finding f;
    f.name = "camera_image";
    f.klass = dc_device::esp32::PeripheralClass::Signal;
    f.verdict = dc_device::esp32::Verdict::Unverified;
    f.evidence = "no picture has been taken since boot";
    return f;
}

void camera_boot_probe(void*) {
    vTaskDelay(pdMS_TO_TICKS(8000));  // after Wi-Fi and the first self checks
    camera_probe_now();
    vTaskDelete(nullptr);
}

InvokeResult camera_capture(const json::Value& args) {
    std::string size_name = "qvga";
    if (const json::Value* v = args.find("size")) size_name = v->string;
    int quality = 60;
    if (const json::Value* v = args.find("quality")) quality = static_cast<int>(v->number);
    if (size_name != "qqvga" && size_name != "qvga" && size_name != "vga") {
        return InvokeResult::invalid("size must be qqvga, qvga or vga");
    }
    if (quality < 10 || quality > 90) return InvokeResult::invalid("quality must be 10..90");
    if (g_camera_busy.exchange(true)) return InvokeResult::failed("Another capture is running", "camera_busy");
    const framesize_t size = size_name == "qqvga" ? FRAMESIZE_QQVGA : size_name == "vga" ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
    // The driver's scale is 0 (best) .. 63; the contract's is 10..90 (best last).
    auto driver_quality = [](int q) { return std::max(4, 63 - q * 63 / 100); };
    std::vector<uint8_t> jpeg;
    int w = 0, h = 0;
    InvokeResult r = camera_take(size, driver_quality(quality), jpeg, w, h);
    if (r.status == InvokeResult::Status::Ok && jpeg.size() > kMaxJpegBytes) {
        r = camera_take(size, driver_quality(std::max(10, quality - 30)), jpeg, w, h);
        if (r.status == InvokeResult::Status::Ok && jpeg.size() > kMaxJpegBytes) {
            r = InvokeResult::failed("The picture is larger than 100000 bytes even at low quality", "image_too_large");
        }
    }
    g_camera_busy = false;
    if (r.status != InvokeResult::Status::Ok) return r;
    uint8_t digest[32];
    mbedtls_sha256(jpeg.data(), jpeg.size(), digest, 0);
    static const char* hex = "0123456789abcdef";
    std::string sha;
    for (uint8_t b : digest) { sha += hex[b >> 4]; sha += hex[b & 15]; }
    std::string out = "{\"content_type\":\"image/jpeg\",\"width\":" + std::to_string(w) + ",\"height\":" + std::to_string(h) +
                      ",\"bytes\":" + std::to_string(jpeg.size()) + ",\"sha256\":\"" + sha + "\",\"data_base64\":\"" +
                      dc_device::base64_encode(jpeg.data(), jpeg.size()) + "\"}";
    return InvokeResult::success(std::move(out));
}
}  // namespace

static const char* TAG = "dc-device";
static Endpoint endpoint("camera_board_01", "camera_board", "ESP32-S3 Camera Board");
static dc_device::esp32::HttpServer http_server(endpoint);
#ifndef DC_DEVICE_VERSION
#define DC_DEVICE_VERSION "0.1.0+dev"  // release builds pass 0.1.0+<git commit> (scripts/build-release.sh)
#endif

static dc_device::esp32::SelfCheck self_check;
static dc_device::esp32::WifiManager wifi_manager;
static dc_device::esp32::HttpServerConfig http_config;

static InvokeResult heap_read(const json::Value&) {
    return InvokeResult::success(std::string("{\"free_heap_bytes\":") +
                                 std::to_string(esp_get_free_heap_size()) + "}");
}

#if !CONFIG_DC_DEVICE_RELEASE
// Test fixture for the safety watchdog (docs/safety-kernel-v0.1.md): "holds" a
// pretend output for at most 2 s. Times come from the device's own clock so the
// network does not enter into the measurement.
static volatile bool g_held = false;
static volatile int64_t g_held_at_us = 0;
static volatile int64_t g_released_at_us = 0;
static volatile int g_release_count = 0;

static InvokeResult hold_state(const json::Value&) {
    int64_t held = g_held_at_us;
    int64_t released = g_released_at_us;
    return InvokeResult::success(
        std::string("{\"held\":") + (g_held ? "true" : "false") +
        ",\"release_count\":" + std::to_string(g_release_count) +
        ",\"held_for_ms\":" + std::to_string(released >= held ? (released - held) / 1000 : -1) + "}");
}
#endif

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Parts nothing on the board can observe are declared in /manifest only when the
    // self-check saw them working or a person said, over the USB cable, that they are
    // there (docs/self-check-v0.1.md, "Declaring capabilities"). Needs NVS, so it is made here.
    const std::vector<std::string> declarable = {"speaker", "screen"};
    static dc_device::esp32::DeclaredParts declared_parts(
        declarable, dc_device::esp32::make_nvs_part_storage(declarable));

    // Tests that need a person: they make a sound or light the screen, and nothing
    // on the board can observe the result.
    dc_device::esp32::CapabilitySpec bp;
    bp.name = "speaker.beep";
    bp.description = "Play one short, quiet beep on the speaker. A person must listen for it.";
    bp.kind = "action";
    bp.parameters_json = R"({"type":"object","properties":{}})";
    bp.safety_json = R"({"max_rate_hz":0.5})";
    bp.handler = [](const json::Value&) {
        return beep(880, 300, 2000) ? InvokeResult::success(R"({"played":true})")
                                    : InvokeResult::failed("Could not start the speaker output");
    };
    bp.available = dc_device::esp32::available_when_confirmed(self_check, "speaker", declared_parts);
    bp.expected_typical_ms = 400;  // the tone is 300 ms plus setup
    bp.expected_max_ms = 1500;
    endpoint.add(bp);
    dc_device::esp32::CapabilitySpec sc;
    sc.name = "screen.fill_test";
    sc.description = "Fill the screen with red, green, blue, then black. A person must watch it.";
    sc.kind = "action";
    sc.parameters_json = R"({"type":"object","properties":{}})";
    sc.safety_json = R"({"max_rate_hz":0.2})";
    sc.handler = [](const json::Value&) {
        return screen_test() ? InvokeResult::success(R"({"drawn":true})")
                             : InvokeResult::failed("Could not drive the screen (" + g_screen_error + ")");
    };
    sc.available = dc_device::esp32::available_when_confirmed(self_check, "screen", declared_parts);
    sc.expected_typical_ms = 3400;  // read from the code (320 + 4 x 700 + fills), not measured
    sc.expected_max_ms = 6000;
    endpoint.add(sc);

    const char* rect_props =
        R"("x":{"type":"integer","minimum":0,"maximum":239},"y":{"type":"integer","minimum":0,"maximum":319},)"
        R"("width":{"type":"integer","minimum":1,"maximum":240},"height":{"type":"integer","minimum":1,"maximum":320})";
    dc_device::esp32::CapabilitySpec dr;
    dr.name = "screen.draw_rect";
    dr.description = "Fill a rectangle of the 240 x 320 screen with one color (RGB565, 0..65535).";
    dr.kind = "action";
    dr.parameters_json = std::string(R"({"type":"object","properties":{)") + rect_props +
                         R"(,"color":{"type":"integer","minimum":0,"maximum":65535}},"required":["x","y","width","height","color"]})";
    dr.safety_json = R"({"max_rate_hz":20})";
    dr.handler = screen_draw_rect;
    dr.available = dc_device::esp32::available_when_confirmed(self_check, "screen", declared_parts);
    dr.expected_max_ms = 1000;
    endpoint.add(dr);
    dc_device::esp32::CapabilitySpec wb;
    wb.name = "screen.write_block";
    wb.description =
        "Write pixels into a rectangle of the 240 x 320 screen: width x height x 2 bytes of RGB565 "
        "(high byte first), at most 7680 bytes, as base64 in data_base64.";
    wb.kind = "action";
    wb.parameters_json = std::string(R"({"type":"object","properties":{)") + rect_props +
                         R"(,"data_base64":{"type":"string",)"
                         R"("x_dc_payload":{"encoding":"base64","max_bytes":7680}}},)"
                         R"("required":["x","y","width","height","data_base64"]})";
    wb.safety_json = R"({"max_rate_hz":20})";
    wb.handler = screen_write_block;
    wb.available = dc_device::esp32::available_when_confirmed(self_check, "screen", declared_parts);
    wb.expected_typical_ms = 150;  // a guess: HTTP time dominates; to be measured
    wb.expected_max_ms = 1500;
    endpoint.add(wb);

    dc_device::esp32::CapabilitySpec cap;
    cap.name = "camera.capture";
    cap.description = "Take one picture and return it as a JPEG in data_base64.";
    cap.kind = "sensor";
    cap.parameters_json =
        R"({"type":"object","properties":{"size":{"type":"string","enum":["qqvga","qvga","vga"]},)"
        R"("quality":{"type":"integer","minimum":10,"maximum":90}},"required":[]})";
    cap.returns_json = R"({"type":"object","properties":{"content_type":{"type":"string"},"width":{"type":"integer"},)"
                       R"("height":{"type":"integer"},"bytes":{"type":"integer"},"sha256":{"type":"string"},)"
                       R"("data_base64":{"type":"string","x_dc_payload":{"encoding":"base64","max_bytes":100000}}}})";
    cap.handler = camera_capture;
    cap.available = [] { return camera_last_probe().verdict == dc_device::esp32::Verdict::Ok; };
    cap.expected_typical_ms = 600;
    cap.expected_max_ms = 4000;
    endpoint.add(cap);

#if !CONFIG_DC_DEVICE_RELEASE
    // Debugging aid: try a microphone on given pins. Drives sck and ws.
    dc_device::esp32::CapabilitySpec trial;
    trial.name = "debug.mic_try";
    trial.description = "Debug: read an I2S microphone on the given pins and report the samples.";
    trial.kind = "sensor";
    trial.parameters_json =
        R"({"type":"object","properties":{"sck":{"type":"integer"},"ws":{"type":"integer"},"sd":{"type":"integer"},)"
        R"("format":{"type":"string","enum":["philips","msb"]},"bits":{"type":"integer","enum":[16,32]},)"
        R"("slot":{"type":"string","enum":["auto","left","right"]},"rate_hz":{"type":"integer"},"settle_ms":{"type":"integer"}},"required":["sck","ws","sd"]})";
    trial.handler = [](const json::Value& a) {
        int sck = static_cast<int>(a.find("sck")->number), ws = static_cast<int>(a.find("ws")->number),
            sd = static_cast<int>(a.find("sd")->number);
        std::vector<int> others;
        for (int p : dc_device::esp32::pins_floating()) {
            if (p != sck && p != ws) others.push_back(p);
        }
        dc_device::esp32::MicFormat fmt;
        if (const json::Value* v = a.find("format")) fmt.msb = v->string == "msb";
        if (const json::Value* v = a.find("bits")) fmt.bits = static_cast<int>(v->number) == 16 ? 16 : 32;
        if (const json::Value* v = a.find("slot")) fmt.slot = v->string == "left" ? 1 : v->string == "right" ? 2 : 0;
        if (const json::Value* v = a.find("rate_hz")) fmt.rate_hz = static_cast<int>(v->number);
        if (const json::Value* v = a.find("settle_ms")) fmt.settle_ms = static_cast<int>(v->number);
        dc_device::esp32::MicTrial t = dc_device::esp32::try_mic(sck, ws, sd, others, fmt);
        std::string preview;
        for (int32_t w : t.preview) preview += (preview.empty() ? "" : ",") + std::to_string(w >> 8);
        std::string moving;
        for (int p : t.moving_pins) moving += (moving.empty() ? "" : ",") + std::to_string(p);
        std::string edges;
        for (auto& pc : t.transitions) edges += (edges.empty() ? "" : ",") + std::string("\"") + std::to_string(pc.first) + "\":" + std::to_string(pc.second);
        char buf[900];
        snprintf(buf, sizeof(buf),
                 "{\"read_ok\":%s,\"samples\":%u,\"constant\":%s,\"rms\":%.5f,\"low_byte_zero\":%.3f,"
                 "\"looks_like_mic\":%s,\"left_ac_rms\":%.5f,\"right_ac_rms\":%.5f,"
                 "\"preview\":[%s],\"moving_pins\":[%s],\"transitions\":{%s}}",
                 t.read_ok ? "true" : "false", static_cast<unsigned>(t.samples), t.constant ? "true" : "false", t.rms,
                 t.low_byte_zero, t.looks_like_mic ? "true" : "false", t.left_ac_rms, t.right_ac_rms,
                 preview.c_str(), moving.c_str(), edges.c_str());
        return InvokeResult::success(buf);
    };
    endpoint.add(trial);

    // Debugging aid: start the camera driver with the given settings, try to get a frame,
    // report what happened, and stop the driver again. The wiring is the board's table.
    dc_device::esp32::CapabilitySpec cam;
    cam.name = "debug.camera_try";
    cam.description = "Debug: start the camera driver with the given settings, take one frame, report, stop.";
    cam.kind = "sensor";
    cam.parameters_json =
        R"({"type":"object","properties":{"xclk_pin":{"type":"integer"},"xclk_hz":{"type":"integer"},)"
        R"("format":{"type":"string","enum":["jpeg","rgb565"]},"size":{"type":"string","enum":["qqvga","qvga","vga"]},)"
        R"("fb_count":{"type":"integer"},"psram":{"type":"boolean"},"latest":{"type":"boolean"},)"
        R"("quality":{"type":"integer"},"frames":{"type":"integer"}},"required":[]})";
    cam.expected_max_ms = 15000;
    cam.handler = [](const json::Value& a) {
        auto geti = [&](const char* k, int d) { const json::Value* v = a.find(k); return v ? static_cast<int>(v->number) : d; };
        auto gets = [&](const char* k, const char* d) { const json::Value* v = a.find(k); return v ? v->string : std::string(d); };
        auto getb = [&](const char* k, bool d) { const json::Value* v = a.find(k); return v ? v->boolean : d; };
        camera_config_t c{};
        c.pin_pwdn = -1; c.pin_reset = -1;
        c.pin_xclk = geti("xclk_pin", -1);
        c.pin_sccb_sda = 4; c.pin_sccb_scl = 5;
        c.pin_d0 = 11; c.pin_d1 = 9; c.pin_d2 = 8; c.pin_d3 = 10;
        c.pin_d4 = 12; c.pin_d5 = 18; c.pin_d6 = 17; c.pin_d7 = 16;
        c.pin_vsync = 6; c.pin_href = 7; c.pin_pclk = 13;
        c.xclk_freq_hz = geti("xclk_hz", 24000000);
        c.ledc_timer = LEDC_TIMER_0; c.ledc_channel = LEDC_CHANNEL_0;
        const bool jpeg = gets("format", "jpeg") == "jpeg";
        c.pixel_format = jpeg ? PIXFORMAT_JPEG : PIXFORMAT_RGB565;
        const std::string size = gets("size", "qvga");
        c.frame_size = size == "qqvga" ? FRAMESIZE_QQVGA : size == "vga" ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
        c.jpeg_quality = geti("quality", 15);
        c.fb_count = geti("fb_count", 2);
        c.fb_location = getb("psram", true) ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
        c.grab_mode = getb("latest", true) ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
        c.sccb_i2c_port = -1;
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t e = esp_camera_init(&c);
        char buf[700];
        if (e != ESP_OK) {
            snprintf(buf, sizeof(buf), "{\"init\":\"%s\",\"psram_bytes\":%u}", esp_err_to_name(e),
                     static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
            return InvokeResult::success(buf);
        }
        const int64_t t_init = esp_timer_get_time() - t0;
        int got = 0, first_len = 0, w = 0, h = 0;
        char head[24] = "";
        int64_t t_first = 0;
        const int frames = geti("frames", 3);
        for (int i = 0; i < frames; ++i) {
            const int64_t t1 = esp_timer_get_time();
            camera_fb_t* fb = esp_camera_fb_get();
            if (fb == nullptr) break;
            ++got;
            if (got == 1) {
                t_first = esp_timer_get_time() - t1;
                first_len = static_cast<int>(fb->len); w = fb->width; h = fb->height;
                snprintf(head, sizeof(head), "%02x%02x%02x%02x", fb->buf[0], fb->buf[1], fb->buf[2], fb->buf[3]);
            }
            esp_camera_fb_return(fb);
        }
        sensor_t* s = esp_camera_sensor_get();
        const int pid = s ? s->id.PID : -1;
        esp_camera_deinit();
        snprintf(buf, sizeof(buf),
                 "{\"init\":\"ok\",\"sensor_pid\":%d,\"init_ms\":%d,\"frames_asked\":%d,\"frames_got\":%d,"
                 "\"first_frame_ms\":%d,\"len\":%d,\"width\":%d,\"height\":%d,\"head\":\"%s\",\"psram_bytes\":%u}",
                 pid, static_cast<int>(t_init / 1000), frames, got, static_cast<int>(t_first / 1000), first_len, w, h,
                 head, static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
        return InvokeResult::success(buf);
    };
    endpoint.add(cam);

    // Registered first: the safe state must exist before a time-limited capability.
    dc_device::esp32::CapabilitySpec release;
    release.name = "debug.release";
    release.description = "Test fixture: end the pretend output. This is the safe state.";
    release.kind = "action";
    release.parameters_json = R"({"type":"object","properties":{}})";
    release.safety_json = R"({"emergency":true})";
    release.handler = [](const json::Value&) {
        g_released_at_us = esp_timer_get_time();
        g_held = false;
        g_release_count = g_release_count + 1;
        return InvokeResult::success(R"({"released":true})");
    };
    endpoint.add(release);
    endpoint.set_safe_state("debug.release");
    dc_device::esp32::CapabilitySpec hold;
    hold.name = "debug.hold";
    hold.description = "Test fixture: start a pretend output that the device must end by itself after 2 s.";
    hold.kind = "action";
    hold.parameters_json = R"({"type":"object","properties":{}})";
    hold.safety_json = R"({"motion":true,"max_duration_ms":2000})";
    hold.handler = [](const json::Value&) {
        g_held_at_us = esp_timer_get_time();
        g_held = true;
        return InvokeResult::success(R"({"held":true})");
    };
    endpoint.add(hold);
    dc_device::esp32::CapabilitySpec hold_read;
    hold_read.name = "debug.hold_state";
    hold_read.description = "Test fixture: whether the pretend output is on, and how long the last one lasted.";
    hold_read.kind = "sensor";
    hold_read.parameters_json = R"({"type":"object","properties":{}})";
    hold_read.handler = hold_state;
    endpoint.add(hold_read);
#endif  // !CONFIG_DC_DEVICE_RELEASE

    dc_device::esp32::CapabilitySpec heap;
    heap.name = "heap.read";
    heap.description = "Read the free heap size.";
    heap.kind = "sensor";
    heap.parameters_json = R"({"type":"object","properties":{}})";
    heap.handler = heap_read;
    endpoint.add(heap);

    // This firmware runs on boards with different parts, so a search that finds
    // nothing is information, not a fault. The host knows what each board should have.
    auto optional = [](dc_device::esp32::Detector d) {
        d.required = false;
        return d;
    };
    // A known map is only a starting point: it is checked by reading the sensor.
    #if CONFIG_DC_DEVICE_CAMERA_KNOWN_MAP
    self_check.add(optional(dc_device::esp32::make_i2c_detector({{4, 5}})));
#else
    self_check.add(optional(dc_device::esp32::make_i2c_detector({})));
#endif
    self_check.add(optional(dc_device::esp32::make_pin_survey_detector()));
    {
        // Quick answers from the last picture; only a self test takes a new one.
        dc_device::esp32::Detector cam_det;
        cam_det.name = "camera_image";
        cam_det.klass = dc_device::esp32::PeripheralClass::Signal;
        cam_det.run = [](dc_device::esp32::Mode mode) {
            return mode == dc_device::esp32::Mode::Full ? camera_probe_now() : camera_last_probe();
        };
        self_check.add(optional(cam_det));
    }
    // The speaker is the sound source that tells a microphone from a pin that just moves.
    // This makes a self test audible on a board that has a microphone.
    dc_device::esp32::MicStimulus stimulus;
    stimulus.play = [](int hz, int ms) { beep(hz, ms, 2000); };
    self_check.add(optional(dc_device::esp32::make_mic_detector(stimulus)));
    // Nothing on the board can observe these two. The pins are from the board's old
    // program, not measured.
    // The speaker is called working when the microphone heard the tone played on it.
    self_check.add(dc_device::esp32::make_heard_by_detector(
        "speaker", "BCLK 42, WS 39, DOUT 41", "known_map", "speaker.beep", "Did you hear one short beep?",
        self_check, "microphone"));
    self_check.add(dc_device::esp32::make_human_confirmed_detector(
        "screen", "SCK 46, MOSI 3, DC 1, CS 14", "known_map", "screen.fill_test",
        "Did the screen turn red, then green, then blue, then black?"));
    dc_device::esp32::attach_self_check(endpoint, self_check);
    xTaskCreate(camera_boot_probe, "cam_probe", 8192, nullptr, 3, nullptr);

    dc_device::esp32::Identity id;
    id.model = "ESP32-S3 Camera Board";
    id.firmware_version = DC_DEVICE_VERSION;
    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        char serial[13];
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        id.serial = serial;
    }
    dc_device::esp32::SetupInfo setup_info;  // what `hello` says; same values as /identity
    setup_info.device_id = "camera_board_01";
    setup_info.type = "camera_board";
    setup_info.model = id.model;
    setup_info.firmware_version = id.firmware_version;
    setup_info.serial = id.serial;
    endpoint.set_identity(std::move(id));
    dc_device::esp32::register_chip_diagnostics(endpoint);
    endpoint.add_diagnostic("free_heap_bytes", [] { return std::to_string(esp_get_free_heap_size()); });

    if (!endpoint.validate_safety()) {
        ESP_LOGE(TAG, "Time-limited capability without a safe state; not serving");
        return;
    }
    http_config.port = CONFIG_DC_DEVICE_HTTP_PORT;
    http_config.max_body_bytes = 12288;  // a screen.write_block of 7680 bytes is about 10.3 KB of base64

    // Network: whatever was stored by USB setup. A network compiled in is only for
    // development and is left out of release builds.
    std::string dev_ssid, dev_password;
#if !CONFIG_DC_DEVICE_RELEASE
    dev_ssid = CONFIG_DC_DEVICE_WIFI_SSID;
    dev_password = CONFIG_DC_DEVICE_WIFI_PASSWORD;
#endif
    dc_device::esp32::WifiManager::Callbacks callbacks;
    callbacks.on_up = [] { http_server.start(http_config); };           // safe to repeat
    callbacks.on_forgotten = [] { http_server.pause(); };              // the watchdog keeps running
    if (!wifi_manager.begin(dev_ssid, dev_password, callbacks)) {
        ESP_LOGE(TAG, "Wi-Fi could not start");
    }

    // USB setup: serves hello / wifi / forget_wifi on the console port in every
    // network state. Needs no network to answer.
    dc_device::esp32::SetupBackend setup_backend = wifi_manager.backend(http_config.port);
    setup_backend.parts = [] { return declared_parts.list(); };
    setup_backend.declare_part = [](const std::string& part, bool present) {
        switch (declared_parts.set(part, present)) {
            case dc_device::esp32::DeclaredParts::SetResult::Ok: return dc_device::esp32::PartResult::Ok;
            case dc_device::esp32::DeclaredParts::SetResult::UnknownPart: return dc_device::esp32::PartResult::UnknownPart;
            default: return dc_device::esp32::PartResult::Failed;
        }
    };
    static dc_device::esp32::SetupProtocol setup(setup_info, setup_backend, dc_device::esp32::write_setup_line);
    dc_device::esp32::start_setup_console(setup);
}
