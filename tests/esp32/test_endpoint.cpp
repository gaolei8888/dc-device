// Host-side tests for the ESP32 HTTP endpoint core (no ESP-IDF required).
// Run: tests/esp32/run.sh

#include <cstdio>
#include <cstdlib>
#include <string>

#include "dc_device_endpoint.hpp"
#include "dc_device_json.hpp"

using namespace dc_device;
using namespace dc_device::esp32;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        std::string a_ = (actual);                                           \
        std::string e_ = (expected);                                         \
        if (a_ != e_) {                                                      \
            std::printf("FAIL %s:%d\n  actual:   %s\n  expected: %s\n",      \
                        __FILE__, __LINE__, a_.c_str(), e_.c_str());         \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static bool g_led_on = false;
static double g_angle = 0;

static Endpoint make_endpoint() {
    Endpoint ep("robot_car_01", "mobile_robot", "Test Car");

    CapabilitySpec led;
    led.name = "led.set";
    led.description = "Set the status LED.";
    led.kind = "action";
    led.parameters_json =
        R"({"type":"object","properties":{"on":{"type":"boolean"}},"required":["on"]})";
    led.handler = [](const json::Value& args) {
        const json::Value* on = args.find("on");
        if (on == nullptr) return InvokeResult::invalid("Missing required argument: on");
        if (!on->is_bool()) return InvokeResult::invalid("Argument on must be of type boolean");
        g_led_on = on->boolean;
        return InvokeResult::success(std::string("{\"on\":") + (g_led_on ? "true" : "false") + "}");
    };
    CHECK(ep.add(led));

    CapabilitySpec servo;
    servo.name = "servo.pan";
    servo.description = "Rotate the pan servo.";
    servo.kind = "action";
    servo.handler = [](const json::Value& args) {
        const json::Value* angle = args.find("angle");
        if (angle == nullptr || !angle->is_number())
            return InvokeResult::invalid("Argument angle must be of type number");
        if (angle->number < -90 || angle->number > 90)
            return InvokeResult::invalid("Argument angle out of range");
        g_angle = angle->number;
        return InvokeResult::success("{\"angle\":" + json::number(g_angle) + "}");
    };
    CHECK(ep.add(servo));

    CapabilitySpec broken;
    broken.name = "hw.fail";
    broken.description = "Always fails.";
    broken.kind = "action";
    broken.handler = [](const json::Value&) { return InvokeResult::failed("PWM channel busy"); };
    CHECK(ep.add(broken));
    return ep;
}

static void test_json() {
    json::Value v;
    std::string err;

    CHECK(json::parse(R"({"a":[1,2.5,-3e2,true,null],"b":{"c":"x"}})", v, err));
    CHECK(v.is_object());
    CHECK(v.find("a")->items.size() == 5);
    CHECK(v.find("a")->items[2].number == -300.0);
    CHECK(v.find("b")->find("c")->string == "x");
    CHECK(v.find("missing") == nullptr);

    CHECK(json::parse(R"("caf\u00e9 \ud83d\ude00 \n")", v, err));
    CHECK_EQ(v.string, "caf\xC3\xA9 \xF0\x9F\x98\x80 \n");

    const char* bad[] = {"", "{", "[1,]", "{\"a\":}", "{'a':1}", "01", "1.", "-", "tru",
                         "\"abc", "\"\\x\"", "\"\\ud800\"", "{\"a\":1} x", "[1 2]",
                         "\"tab\there\""};
    for (const char* text : bad) {
        CHECK(!json::parse(text, v, err));
        CHECK(!err.empty());
    }

    std::string deep(100, '[');
    CHECK(!json::parse(deep, v, err));

    CHECK_EQ(json::quote("a\"b\\c\n\x01"), "\"a\\\"b\\\\c\\n\\u0001\"");
    CHECK_EQ(json::number(30), "30");
    CHECK_EQ(json::number(-0.0), "0");
    CHECK_EQ(json::number(1.5), "1.5");
    CHECK_EQ(json::number(1.0 / 0.0), "null");
}

static void test_registration() {
    Endpoint ep("id", "t", "n");
    CapabilitySpec ok;
    ok.name = "x.y";
    ok.kind = "sensor";
    ok.handler = [](const json::Value&) { return InvokeResult::success(); };
    CHECK(ep.add(ok));
    CHECK(!ep.add(ok));  // duplicate

    CapabilitySpec bad = ok;
    bad.name = "";
    CHECK(!ep.add(bad));
    bad = ok; bad.name = "k"; bad.kind = "bogus";
    CHECK(!ep.add(bad));
    bad = ok; bad.name = "h"; bad.handler = nullptr;
    CHECK(!ep.add(bad));
    bad = ok; bad.name = "p"; bad.parameters_json = "[1]";
    CHECK(!ep.add(bad));
    bad = ok; bad.name = "q"; bad.safety_json = "{oops";
    CHECK(!ep.add(bad));
}

static void test_routes() {
    Endpoint ep = make_endpoint();

    Response r = ep.handle("GET", "/health", "");
    CHECK(r.status == 200);
    CHECK_EQ(r.body, R"({"status":"ok","device_id":"robot_car_01"})");

    r = ep.handle("GET", "/health?verbose=1", "");
    CHECK(r.status == 200);

    r = ep.handle("GET", "/manifest", "");
    CHECK(r.status == 200);
    json::Value m;
    std::string err;
    CHECK(json::parse(r.body, m, err));
    CHECK_EQ(m.find("spec_version")->string, "0.1");
    CHECK_EQ(m.find("device")->find("id")->string, "robot_car_01");
    CHECK(m.find("device")->find("metadata")->is_object());
    const json::Value* caps = m.find("capabilities");
    CHECK(caps->items.size() == 3);
    CHECK_EQ(caps->items[0].find("name")->string, "led.set");
    CHECK_EQ(caps->items[0].find("kind")->string, "action");
    CHECK(caps->items[0].find("parameters")->find("required")->items[0].string == "on");
    CHECK(caps->items[0].find("returns")->is_object());
    CHECK(caps->items[0].find("safety")->is_object());

    CHECK(ep.handle("GET", "/nope", "").status == 404);
    CHECK(ep.handle("POST", "/health", "").status == 404);
    CHECK(ep.handle("GET", "/invoke", "").status == 404);
    CHECK(ep.handle("DELETE", "/manifest", "").status == 404);
}

static void test_invoke() {
    Endpoint ep = make_endpoint();

    // Whitespace variants that the old substring matcher could not handle.
    Response r = ep.handle("POST", "/invoke",
        "{ \"arguments\" : { \"on\" : true } ,\n \"capability\" : \"led.set\" }");
    CHECK(r.status == 200);
    CHECK_EQ(r.body, R"({"ok":true,"capability":"led.set","result":{"on":true}})");
    CHECK(g_led_on);

    r = ep.handle("POST", "/invoke", R"({"capability":"led.set","arguments":{"on":false}})");
    CHECK(r.status == 200);
    CHECK(!g_led_on);

    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":30}})");
    CHECK(r.status == 200);
    CHECK_EQ(r.body, R"({"ok":true,"capability":"servo.pan","result":{"angle":30}})");
    CHECK(g_angle == 30.0);

    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":-12.5}})");
    CHECK_EQ(r.body, R"({"ok":true,"capability":"servo.pan","result":{"angle":-12.5}})");

    // Handler-level argument errors -> 400
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":91}})");
    CHECK(r.status == 400);
    CHECK(r.body.find("\"InvalidArguments\"") != std::string::npos);
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":"30"}})");
    CHECK(r.status == 400);
    r = ep.handle("POST", "/invoke", R"({"capability":"led.set"})");  // arguments omitted
    CHECK(r.status == 400);
    r = ep.handle("POST", "/invoke", R"({"capability":"led.set","arguments":null})");
    CHECK(r.status == 400);
    CHECK_EQ(r.body, R"({"ok":false,"error":"InvalidArguments","message":"Missing required argument: on"})");

    // Hardware failure -> 500
    r = ep.handle("POST", "/invoke", R"({"capability":"hw.fail"})");
    CHECK(r.status == 500);
    CHECK_EQ(r.body, R"({"ok":false,"error":"DeviceError","message":"PWM channel busy"})");

    // Unknown capability -> 404
    r = ep.handle("POST", "/invoke", R"({"capability":"laser.fire"})");
    CHECK(r.status == 404);
    CHECK(r.body.find("\"UnknownCapability\"") != std::string::npos);

    // Malformed requests -> 400
    const char* bad_bodies[] = {"", "not json", "[]", "{}", R"({"capability":5})",
                                R"({"capability":"led.set","arguments":[]})",
                                R"({"capability":"led.set","arguments":3})"};
    for (const char* body : bad_bodies) {
        r = ep.handle("POST", "/invoke", body);
        CHECK(r.status == 400);
        CHECK(r.body.find("\"InvalidRequest\"") != std::string::npos);
        json::Value parsed;
        std::string err;
        CHECK(json::parse(r.body, parsed, err));  // error bodies are valid JSON
    }

    // Capability names with quotes are escaped in error output.
    r = ep.handle("POST", "/invoke", R"({"capability":"a\"b"})");
    json::Value parsed;
    std::string err;
    CHECK(r.status == 404);
    CHECK(json::parse(r.body, parsed, err));
}

static Endpoint make_health_endpoint(int* sweeps) {
    Endpoint ep("cam_01", "camera", "Test Camera");
    CHECK(ep.add_health_check("connection", [] { return CheckResult::ok(); }));
    CHECK(ep.add_health_check("fps", [] {
        return CheckResult::warning("LOW_FPS", "fps below expected threshold");
    }));
    CHECK(ep.add_self_test("sweep", [sweeps] {
        ++*sweeps;
        return CheckResult::ok();
    }));
    CHECK(ep.add_diagnostic("firmware", [] { return std::string("\"0.1.0\""); }));
    CHECK(ep.add_diagnostic("heap", [] { return std::string("{\"free\":1234}"); }));
    CHECK(ep.add_diagnostic("bad", [] { return std::string("{oops"); }));
    return ep;
}

static void test_health_registration() {
    Endpoint ep("id", "t", "n");
    auto ok = [] { return CheckResult::ok(); };
    CHECK(ep.add_health_check("a", ok));
    CHECK(!ep.add_health_check("a", ok));  // duplicate
    CHECK(!ep.add_self_test("a", ok));     // duplicate across registries
    CHECK(!ep.add_health_check("", ok));
    CHECK(!ep.add_health_check("b", nullptr));
    CHECK(!ep.add_diagnostic("", [] { return std::string("1"); }));
    CHECK(ep.add_diagnostic("d", [] { return std::string("1"); }));
    CHECK(!ep.add_diagnostic("d", [] { return std::string("2"); }));
}

static void test_health_routes() {
    Endpoint empty("dev1", "t", "n");
    CHECK_EQ(empty.handle("GET", "/status", "").body,
             R"({"device_id":"dev1","status":"ok","checks":{},"issues":[]})");

    int sweeps = 0;
    Endpoint ep = make_health_endpoint(&sweeps);

    // /health stays plain liveness even when a check warns.
    CHECK_EQ(ep.handle("GET", "/health", "").body, R"({"status":"ok","device_id":"cam_01"})");

    Response r = ep.handle("GET", "/status", "");
    CHECK(r.status == 200);
    CHECK_EQ(r.body,
        R"({"device_id":"cam_01","status":"degraded","checks":{"connection":"ok","fps":"warning"},)"
        R"("issues":[{"code":"LOW_FPS","severity":"warning","check":"fps",)"
        R"("message":"fps below expected threshold"}]})");
    CHECK(sweeps == 0);  // /status never runs self tests

    r = ep.handle("GET", "/diagnostics", "");
    CHECK(sweeps == 0);  // nor does /diagnostics
    json::Value d;
    std::string err;
    CHECK(json::parse(r.body, d, err));
    CHECK_EQ(d.find("device_id")->string, "cam_01");
    CHECK_EQ(d.find("status")->string, "degraded");
    CHECK(d.find("uptime_s")->number >= 0);
    CHECK(d.find("capability_count")->number == 0);
    CHECK(d.find("last_self_test")->is_null());
    CHECK_EQ(d.find("info")->find("firmware")->string, "0.1.0");
    CHECK(d.find("info")->find("heap")->find("free")->number == 1234);
    CHECK(d.find("info")->find("bad")->find("error") != nullptr);  // contained

    r = ep.handle("POST", "/self_test", "");
    CHECK(r.status == 200);
    CHECK(sweeps == 1);
    CHECK(json::parse(r.body, d, err));
    CHECK_EQ(d.find("status")->string, "degraded");
    CHECK_EQ(d.find("checks")->find("sweep")->string, "ok");
    CHECK(d.find("checks")->keys.size() == 3);

    // The self-test report is now remembered by diagnostics.
    std::string self_test_body = r.body;
    r = ep.handle("GET", "/diagnostics", "");
    CHECK(json::parse(r.body, d, err));
    CHECK(d.find("last_self_test")->is_object());
    CHECK_EQ(d.find("last_self_test")->find("checks")->find("sweep")->string, "ok");
    CHECK(sweeps == 1);

    // Wrong methods are 404, like the other routes.
    CHECK(ep.handle("POST", "/status", "").status == 404);
    CHECK(ep.handle("POST", "/diagnostics", "").status == 404);
    CHECK(ep.handle("GET", "/self_test", "").status == 404);
}

static void test_health_states() {
    Endpoint ep("dev", "t", "n");
    ep.add_health_check("w", [] { return CheckResult::warning("", ""); });
    ep.add_health_check("s", [] { return CheckResult::skipped(); });
    json::Value v;
    std::string err;
    CHECK(json::parse(ep.status(), v, err));
    CHECK_EQ(v.find("status")->string, "degraded");
    CHECK_EQ(v.find("checks")->find("s")->string, "skipped");
    CHECK(v.find("issues")->items.size() == 1);  // skipped raises no issue
    CHECK_EQ(v.find("issues")->items[0].find("code")->string, "CHECK_WARNING");
    CHECK(!v.find("issues")->items[0].find("message")->string.empty());

    ep.add_health_check("f", [] { return CheckResult::failed("NO_STREAM", "gone \"away\""); });
    CHECK(json::parse(ep.status(), v, err));  // quotes in messages stay valid JSON
    CHECK_EQ(v.find("status")->string, "failed");  // failed beats warning
    CHECK_EQ(v.find("issues")->items[1].find("severity")->string, "error");
    CHECK_EQ(v.find("issues")->items[1].find("message")->string, "gone \"away\"");
}

int main() {
    test_health_registration();
    test_health_routes();
    test_health_states();
    test_json();
    test_registration();
    test_routes();
    test_invoke();
    if (g_failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
