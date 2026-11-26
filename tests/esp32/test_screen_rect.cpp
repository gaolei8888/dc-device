// Host-side tests for Base64 and the screen.draw_rect argument check (no ESP-IDF).
// Run: tests/esp32/run_screen_rect.sh

#include <cstdio>
#include <string>
#include <vector>

#include "dc_device_base64.hpp"
#include "dc_device_json.hpp"
#include "dc_device_screen_rect.hpp"

using namespace dc_device;
using namespace dc_device::esp32;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static std::string text_of(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

static void test_base64() {
    std::vector<uint8_t> out;
    // RFC 4648 test vectors.
    CHECK(base64_decode("", out) && out.empty());
    CHECK(base64_decode("Zg==", out) && text_of(out) == "f");
    CHECK(base64_decode("Zm8=", out) && text_of(out) == "fo");
    CHECK(base64_decode("Zm9v", out) && text_of(out) == "foo");
    CHECK(base64_decode("Zm9vYg==", out) && text_of(out) == "foob");
    CHECK(base64_decode("Zm9vYmE=", out) && text_of(out) == "fooba");
    CHECK(base64_decode("Zm9vYmFy", out) && text_of(out) == "foobar");
    // All 64 symbols, including + and /.
    CHECK(base64_decode("+/8=", out) && out.size() == 2 && out[0] == 0xFB && out[1] == 0xFF);
    // Refused, and nothing half-decoded is left behind.
    CHECK(!base64_decode("Zm9", out) && out.empty());           // length not a multiple of 4
    CHECK(!base64_decode("Zm9v!A==", out) && out.empty());      // stray character
    CHECK(!base64_decode("Zg=a", out) && out.empty());          // data after padding
    CHECK(!base64_decode("=Zg=", out) && out.empty());          // padding too early
    CHECK(!base64_decode("Zg==Zm9v", out) && out.empty());      // padding in the middle
    CHECK(!base64_decode("Zm9v\n", out) && out.empty());        // whitespace is not accepted
}

static bool parse_args(const std::string& text, ScreenRect& rect, std::string& error) {
    json::Value v;
    std::string perr;
    if (!json::parse(text, v, perr)) {
        error = "test bug: bad JSON: " + perr;
        return false;
    }
    return parse_draw_rect(v, rect, error);
}

static void test_draw_rect() {
    ScreenRect r;
    std::string e;
    // 2x1 pixels = 4 bytes = "AAECAw==" is 00 01 02 03.
    CHECK(parse_args(R"({"x":10,"y":20,"w":2,"h":1,"data":"AAECAw=="})", r, e));
    CHECK(r.x == 10 && r.y == 20 && r.w == 2 && r.h == 1);
    CHECK(r.pixels.size() == 4 && r.pixels[0] == 0 && r.pixels[3] == 3);
    // The whole screen corner to corner is fine.
    CHECK(parse_args(R"({"x":239,"y":319,"w":1,"h":1,"data":"AAA="})", r, e));
    // Refused: outside the screen, empty, fractional, missing, wrong pixel count, bad data.
    CHECK(!parse_args(R"({"x":239,"y":0,"w":2,"h":1,"data":"AAECAw=="})", r, e));
    CHECK(!parse_args(R"({"x":0,"y":319,"w":1,"h":2,"data":"AAECAw=="})", r, e));
    CHECK(!parse_args(R"({"x":-1,"y":0,"w":2,"h":1,"data":"AAECAw=="})", r, e));
    CHECK(!parse_args(R"({"x":0,"y":0,"w":0,"h":1,"data":""})", r, e));
    CHECK(!parse_args(R"({"x":0.5,"y":0,"w":2,"h":1,"data":"AAECAw=="})", r, e));
    CHECK(!parse_args(R"({"x":0,"y":0,"w":2,"h":1})", r, e));
    CHECK(!parse_args(R"({"x":0,"y":0,"w":2,"h":1,"data":5})", r, e));
    CHECK(!parse_args(R"({"x":0,"y":0,"w":2,"h":1,"data":"AAEC"})", r, e));      // 3 bytes, needs 4
    CHECK(!parse_args(R"({"x":0,"y":0,"w":2,"h":1,"data":"AAECAwQF"})", r, e));  // 6 bytes, needs 4
    CHECK(!parse_args(R"({"x":0,"y":0,"w":2,"h":1,"data":"not base64!"})", r, e));
    CHECK(!e.empty());  // a refusal always says why
}

int main() {
    test_base64();
    test_draw_rect();
    if (g_failures == 0) std::printf("screen_rect: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
