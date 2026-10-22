// Host-side tests for the ESP32 HTTP endpoint core (no ESP-IDF required).
// Run: tests/esp32/run.sh

#include <array>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>

#include "dc_device_access.hpp"
#include "dc_device_audio_analysis.hpp"
#include "dc_device_chip.hpp"
#include "dc_device_endpoint.hpp"
#include "dc_device_i2c_ids.hpp"
#include "dc_device_pins.hpp"
#include "dc_device_selfcheck.hpp"
#include "dc_device_setup.hpp"
#include "dc_device_sha256.hpp"
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
    servo.parameters_json =
        R"({"type":"object","properties":{"angle":{"type":"number","minimum":-90,"maximum":90},"mode":{"enum":["fast","slow",1]},"n":{"type":"integer"}},"required":["angle"]})";
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

    // Schema enforcement happens before the handler runs.
    g_angle = 0;
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":-91}})");
    CHECK_EQ(r.body, R"({"ok":false,"error":"InvalidArguments","message":"Argument angle must be >= -90"})");
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":91}})");
    CHECK_EQ(r.body, R"({"ok":false,"error":"InvalidArguments","message":"Argument angle must be <= 90"})");
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{}})");
    CHECK_EQ(r.body, R"({"ok":false,"error":"InvalidArguments","message":"Missing required argument: angle"})");
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":1,"mode":"warp"}})");
    CHECK(r.status == 400);
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":1,"mode":"slow","n":2}})");
    CHECK(r.status == 200);
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":1,"mode":1}})");
    CHECK(r.status == 200);
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":1,"n":1.5}})");
    CHECK_EQ(r.body, R"({"ok":false,"error":"InvalidArguments","message":"Argument n must be of type integer"})");
    r = ep.handle("POST", "/invoke", R"({"capability":"servo.pan","arguments":{"angle":2,"extra":true}})");
    CHECK(r.status == 200);  // unknown arguments pass, as in the Python SDK

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

static void test_rate_limit() {
    Endpoint ep("d", "t", "n");
    double now = 100.0;
    ep.set_clock([&now] { return now; });
    int calls = 0;
    auto make = [&](const char* name, const char* safety) {
        CapabilitySpec spec;
        spec.name = name;
        spec.kind = "action";
        spec.safety_json = safety;
        spec.handler = [&calls](const json::Value&) { ++calls; return InvokeResult::success(); };
        return spec;
    };
    CHECK(ep.add(make("act", R"({"max_rate_hz":2})")));
    CHECK(ep.add(make("stop", R"({"max_rate_hz":1,"emergency":true})")));
    CHECK(!ep.add(make("bad0", R"({"max_rate_hz":0})")));
    CHECK(!ep.add(make("bad1", R"({"max_rate_hz":"fast"})")));
    CHECK(!ep.add(make("bad2", R"({"emergency":1})")));
    CHECK(!ep.add(make("bad3", R"({"emergency":true,"requires_confirmation":true})")));
    CHECK(ep.add(make("ok3", R"({"requires_confirmation":true})")));

    const char* act = R"({"capability":"act"})";
    CHECK(ep.handle("POST", "/invoke", act).status == 200);
    now += 0.1;
    Response r = ep.handle("POST", "/invoke", act);
    CHECK(r.status == 429);
    CHECK(r.body.find("\"RateLimited\"") != std::string::npos);
    CHECK(calls == 1);
    now += 0.5;  // 0.6 s since the accepted call, limit is 0.5 s
    CHECK(ep.handle("POST", "/invoke", act).status == 200);

    for (int i = 0; i < 4; ++i) {
        CHECK(ep.handle("POST", "/invoke", R"({"capability":"stop"})").status == 200);
    }
}

static void test_safety_kernel() {
    Endpoint ep("d", "t", "n");
    double now = 0;
    ep.set_clock([&now] { return now; });
    std::string log;
    auto make = [&](const char* name, const char* safety) {
        CapabilitySpec spec;
        spec.name = name;
        spec.kind = "action";
        spec.safety_json = safety;
        spec.parameters_json = R"({"type":"object","properties":{},"required":[]})";
        spec.handler = [&log, name](const json::Value&) { log += name; log += ';'; return InvokeResult::success(); };
        return spec;
    };
    CHECK(ep.add(make("move", R"({"max_duration_ms":500})")));
    CHECK(!ep.add(make("bad0", R"({"max_duration_ms":0})")));
    CHECK(!ep.add(make("bad1", R"({"max_duration_ms":1.5})")));
    CHECK(!ep.add(make("bad2", R"({"max_duration_ms":5,"emergency":true})")));
    CHECK(!ep.validate_safety());  // time limited, no safe state yet
    CHECK(ep.add(make("stop", R"({"emergency":true})")));

    // A moving capability also needs a safe state.
    Endpoint mover("d", "t", "n");
    CHECK(mover.validate_safety());
    CHECK(!mover.add(make("m0", R"({"motion":"yes"})")));
    CHECK(mover.add(make("arm", R"({"motion":true})")));
    CHECK(!mover.validate_safety());
    CHECK(mover.add(make("arm.stop", R"({"emergency":true})")));
    CHECK(mover.set_safe_state("arm.stop"));
    CHECK(mover.validate_safety());

    CHECK(!ep.set_safe_state("move"));     // not an emergency capability
    CHECK(!ep.set_safe_state("missing"));
    CHECK(ep.set_safe_state("stop", true));
    CHECK(ep.validate_safety());
    CHECK(ep.manifest().find(R"("safety_kernel":{"safe_state":"stop","safe_on_boot":true})") != std::string::npos);

    const char* move = R"({"capability":"move"})";
    ep.handle("POST", "/invoke", move);
    now = 0.4;
    CHECK(!ep.tick());
    ep.handle("POST", "/invoke", move);  // re-invoking extends the deadline
    now = 0.8;
    CHECK(!ep.tick());
    now = 1.0;
    CHECK(ep.tick());
    CHECK_EQ(log, "move;move;stop;");
    CHECK(!ep.tick());

    ep.handle("POST", "/invoke", move);
    CHECK(ep.time_limited_running());
    // A self test would hold the endpoint past the time limit, so it is refused.
    Response busy = ep.handle("POST", "/self_test", "");
    CHECK(busy.status == 409);
    CHECK(busy.body.find("\"Busy\"") != std::string::npos);
    ep.handle("POST", "/invoke", R"({"capability":"stop"})");
    CHECK(!ep.time_limited_running());
    CHECK(ep.handle("POST", "/self_test", "").status == 200);
    now = 9.0;
    CHECK(!ep.tick());  // stopping by hand disarms
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

static void test_identity() {
    Endpoint ep("robot_car_01", "mobile_robot", "Car");

    // Defaults: only the required fields.
    Response r = ep.handle("GET", "/identity", "");
    CHECK(r.status == 200);
    CHECK_EQ(r.body,
        R"({"spec_version":"0.1","device_id":"robot_car_01","type":"mobile_robot","name":"Car"})");

    Identity id;
    id.manufacturer = "Acme";
    id.model = "RC-1";
    id.hardware_revision = "B";
    id.firmware_version = "0.1.0";
    id.serial = "AABBCCDDEEFF";
    id.profile_id = "esp32-s3-robot-car";
    id.profile_version = "0.1.0";
    CHECK(ep.set_identity(id));

    r = ep.handle("GET", "/identity", "");
    json::Value v;
    std::string err;
    CHECK(json::parse(r.body, v, err));
    CHECK_EQ(v.find("device_id")->string, "robot_car_01");
    CHECK_EQ(v.find("manufacturer")->string, "Acme");
    CHECK_EQ(v.find("serial")->string, "AABBCCDDEEFF");
    CHECK_EQ(v.find("profile")->find("id")->string, "esp32-s3-robot-car");
    CHECK_EQ(v.find("profile")->find("version")->string, "0.1.0");

    // Identity agrees with the manifest's device block.
    CHECK(json::parse(ep.manifest(), v, err));
    CHECK_EQ(v.find("device")->find("id")->string, "robot_car_01");
    CHECK(v.find("device")->find("serial") == nullptr);  // identity stays out of the manifest

    // Empty fields are omitted, not emitted as "".
    Identity partial;
    partial.model = "RC-1";
    CHECK(ep.set_identity(partial));
    CHECK(json::parse(ep.identity(), v, err));
    CHECK(v.find("manufacturer") == nullptr);
    CHECK(v.find("profile") == nullptr);
    CHECK_EQ(v.find("model")->string, "RC-1");

    // Strings are escaped.
    Identity quoted;
    quoted.model = "7\" \\ board";
    CHECK(ep.set_identity(quoted));
    CHECK(json::parse(ep.identity(), v, err));
    CHECK_EQ(v.find("model")->string, "7\" \\ board");

    // Rejected profile references leave the previous identity in place.
    const char* bad[][2] = {
        {"p", ""}, {"", "0.1.0"},                  // only one half
        {"Bad Id", "0.1.0"}, {"-p", "0.1.0"}, {"p/q", "0.1.0"},
        {"p", "1"}, {"p", "1.2"}, {"p", "1.2.3.4"}, {"p", "1..3"}, {"p", "a.b.c"}, {"p", "1.2."},
    };
    for (auto& pair : bad) {
        Identity b;
        b.profile_id = pair[0];
        b.profile_version = pair[1];
        CHECK(!ep.set_identity(b));
    }
    CHECK(json::parse(ep.identity(), v, err));
    CHECK_EQ(v.find("model")->string, "7\" \\ board");

    CHECK(ep.handle("POST", "/identity", "").status == 404);

    // Valid edge cases.
    Identity ok;
    ok.profile_id = "a.b_c-1";
    ok.profile_version = "10.20.30";
    CHECK(ep.set_identity(ok));
}

static void test_chip_facts() {
    dc_device::esp32::ChipFacts f;
    f.model = "esp32s3";
    f.revision = "v0.2";
    f.cores = 2;
    f.features = {"wifi", "ble"};
    f.flash_bytes = 16777216;
    f.mac = "B8F862E96740";
    f.idf_version = "v5.4.4";
    f.reset_reason = "poweron";
    // PSRAM was not reported: null, not zero.
    CHECK_EQ(dc_device::esp32::chip_facts_json(f),
        R"({"model":"esp32s3","revision":"v0.2","cores":2,"features":["wifi","ble"],)"
        R"("flash_bytes":16777216,"psram_bytes":null,"mac":"B8F862E96740",)"
        R"("idf_version":"v5.4.4","reset_reason":"poweron"})");
    f.psram_bytes = 0;  // reported as really absent
    f.flash_bytes = -1;
    json::Value v;
    std::string err;
    CHECK(json::parse(dc_device::esp32::chip_facts_json(f), v, err));
    CHECK(v.find("psram_bytes") != nullptr && v.find("psram_bytes")->number == 0);
    CHECK(v.find("flash_bytes") != nullptr && v.find("flash_bytes")->type == json::Value::Type::Null);
}

static void test_selfcheck() {
    using namespace dc_device::esp32;
    SelfCheck sc;
    int full_runs = 0, quick_runs = 0;
    // A bus device that is only found by scanning: not found in Quick mode.
    bool located = false;
    CHECK(sc.add({"camera", PeripheralClass::Bus, [&](Mode m) {
        (m == Mode::Full ? full_runs : quick_runs)++;
        if (!located && m == Mode::Full) located = true;  // scan finds it
        if (!located) return Finding{Verdict::NotFound, "nothing at the known pins", "", ""};
        return m == Mode::Full ? Finding{Verdict::Ok, "chip id 0x2642", "SDA 4, SCL 5", "scan"}
                                : Finding{Verdict::Ok, "chip id 0x2642", "", ""};
    }}));
    CHECK(sc.add({"led", PeripheralClass::NoSignal, [](Mode) {
        return Finding{Verdict::Unverified, "no feedback", "GPIO 2", ""};
    }}));
    CHECK(!sc.add({"led", PeripheralClass::NoSignal, [](Mode) { return Finding{}; }}));  // duplicate
    CHECK(!sc.add({"", PeripheralClass::Bus, [](Mode) { return Finding{}; }}));          // no name
    CHECK(!sc.add({"x", PeripheralClass::Bus, nullptr}));                                 // no function
    CHECK(sc.size() == 2);

    // Before any run every verdict is not_run.
    CHECK_EQ(sc.report_json(),
        R"({"ran":false,"mode":"quick","peripherals":[)"
        R"({"name":"camera","class":"bus","verdict":"not_run","evidence":"","wiring":"","source":""},)"
        R"({"name":"led","class":"no_signal","verdict":"not_run","evidence":"","wiring":"","source":""}]})");

    Endpoint ep("dev", "t", "Dev");
    CHECK(attach_self_check(ep, sc));
    CHECK(!attach_self_check(ep, sc));  // names already taken

    // /status runs Quick only: the camera is not found, which is a warning.
    Response r = ep.handle("GET", "/status", "");
    CHECK(full_runs == 0 && quick_runs == 1);
    CHECK(r.body.find("\"status\":\"degraded\"") != std::string::npos);
    CHECK(r.body.find("PERIPHERAL_NOT_FOUND") != std::string::npos);
    // An unverified peripheral alone does not degrade the device.
    CHECK(r.body.find("led") == std::string::npos);

    // /self_test runs Full, finds the camera, and the device is healthy.
    r = ep.handle("POST", "/self_test", "");
    CHECK(full_runs == 1);
    CHECK(r.body.find("\"status\":\"ok\"") != std::string::npos);

    // The evidence and the wiring are in /diagnostics, including the unverified LED.
    r = ep.handle("GET", "/diagnostics", "");
    CHECK(r.body.find("\"verdict\":\"unverified\"") != std::string::npos);
    CHECK(r.body.find("\"wiring\":\"SDA 4, SCL 5\"") != std::string::npos);
    CHECK(r.body.find("\"source\":\"scan\"") != std::string::npos);

    // Afterwards /status finds it again without scanning.
    r = ep.handle("GET", "/status", "");
    CHECK(full_runs == 1 && quick_runs == 4);  // each of /status, /self_test, /diagnostics, /status ran one Quick
    CHECK(r.body.find("\"status\":\"ok\"") != std::string::npos);
    CHECK(r.body.find("\"status\":\"ok\"") != std::string::npos);

    // A failing detector makes the device failed and names it.
    SelfCheck bad;
    bad.add({"mic", PeripheralClass::Signal, [](Mode) {
        return Finding{Verdict::Failed, "samples constant", "", ""};
    }});
    Endpoint ep2("dev2", "t", "Dev2");
    attach_self_check(ep2, bad);
    r = ep2.handle("GET", "/status", "");
    CHECK(r.body.find("\"status\":\"failed\"") != std::string::npos);
    CHECK(r.body.find("PERIPHERAL_FAILED") != std::string::npos);
    CHECK(r.body.find("mic (samples constant)") != std::string::npos);

    // A detector that reports several peripherals, each naming itself.
    SelfCheck multi;
    int scans = 0;
    CHECK(multi.add({"i2c", PeripheralClass::Bus, nullptr, [&](Mode m) {
        ++scans;
        std::vector<Finding> v;
        Finding a{Verdict::Ok, "mpu6050, WHO_AM_I 0x68", "SDA 8, SCL 9", m == Mode::Full ? "scan" : "", "imu", PeripheralClass::Bus};
        Finding b{Verdict::Ok, "device at 0x76", "SDA 8, SCL 9", "", "i2c:0x76", PeripheralClass::Bus};
        v.push_back(a);
        v.push_back(b);
        return v;
    }}));
    CHECK(!multi.add({"both", PeripheralClass::Bus, [](Mode) { return Finding{}; },
                      [](Mode) { return std::vector<Finding>{}; }}));  // exactly one of the two
    CHECK(!multi.add({"neither", PeripheralClass::Bus, nullptr, nullptr}));
    multi.run(Mode::Full);
    multi.run(Mode::Quick);  // keeps how the wiring was found
    CHECK(scans == 2);
    std::vector<Entry> rows = multi.entries();
    CHECK(rows.size() == 2);
    CHECK(rows[0].name == "imu" && rows[1].name == "i2c:0x76");
    CHECK(rows[0].finding.source == "scan");
    CHECK(multi.report_json().find("\"name\":\"imu\"") != std::string::npos);
    // Found nothing: one row, so the report still shows it ran.
    SelfCheck none;
    none.add({"i2c", PeripheralClass::Bus, nullptr, [](Mode) { return std::vector<Finding>{}; }});
    none.run(Mode::Quick);
    CHECK(none.entries().size() == 1 && none.entries()[0].finding.verdict == Verdict::NotFound);

    // A part that may be absent: not_found is reported, marked optional, and does not
    // degrade the device.
    {
        SelfCheck opt;
        Detector mic{"mic", PeripheralClass::Signal,
                     [](Mode) { return Finding{Verdict::NotFound, "nothing answered", "", ""}; }};
        mic.required = false;
        CHECK(opt.add(mic));
        Endpoint oep("dev", "t", "Dev");
        CHECK(attach_self_check(oep, opt));
        Response os = oep.handle("GET", "/status", "");
        CHECK(os.body.find("\"status\":\"ok\"") != std::string::npos);
        CHECK(opt.report_json().find("\"verdict\":\"not_found\"") != std::string::npos);
        CHECK(opt.report_json().find("\"optional\":true") != std::string::npos);
    }
    // A part that answered earlier and is silent now is a fault, optional or not.
    {
        bool present = true;
        SelfCheck gone;
        Detector mic{"mic", PeripheralClass::Signal, [&present](Mode) {
                         return present ? Finding{Verdict::Ok, "answers a tone", "", ""}
                                        : Finding{Verdict::NotFound, "nothing answered", "", ""};
                     }};
        mic.required = false;
        gone.add(mic);
        gone.run(Mode::Quick);
        CHECK(gone.entries()[0].finding.verdict == Verdict::Ok);
        present = false;
        gone.run(Mode::Quick);
        CHECK(gone.entries()[0].finding.verdict == Verdict::Failed);
        CHECK(gone.entries()[0].finding.evidence.find("answered earlier") != std::string::npos);
        Endpoint gep("dev", "t", "Dev");
        CHECK(attach_self_check(gep, gone));
        CHECK(gep.handle("GET", "/status", "").body.find("PERIPHERAL_FAILED") != std::string::npos);
        // A Full run is a search that may be cut short, so it does not judge.
        present = true;
        gone.run(Mode::Quick);
        present = false;
        gone.run(Mode::Full);
        CHECK(gone.entries()[0].finding.verdict == Verdict::NotFound);
    }
    // A device that disappears from a multi-peripheral detector's list.
    {
        bool imu = true;
        SelfCheck many;
        many.add({"i2c", PeripheralClass::Bus, nullptr, [&imu](Mode) {
                      std::vector<Finding> out;
                      if (imu) {
                          Finding f{Verdict::Ok, "chip id", "", "scan"};
                          f.name = "imu";
                          out.push_back(f);
                      }
                      return out;
                  }});
        many.run(Mode::Quick);
        imu = false;
        many.run(Mode::Quick);
        std::vector<Entry> mr = many.entries();
        CHECK(mr.size() == 1 && mr[0].name == "imu" && mr[0].finding.verdict == Verdict::Failed);
    }

    // A speaker is working when the microphone heard the tone played on it.
    {
        Verdict mic_verdict = Verdict::Ok;
        bool heard = true;
        SelfCheck pair;
        pair.add({"microphone", PeripheralClass::Signal, [&](Mode) {
                      Finding f{mic_verdict, "answers a tone", "SD 2", "scan"};
                      f.heard_stimulus = heard;
                      return f;
                  }});
        CHECK(pair.add(make_heard_by_detector("speaker", "DOUT 41", "known_map", "speaker.beep",
                                              "Did you hear a beep?", pair, "microphone")));
        pair.run(Mode::Quick);
        const Finding* sp = pair.finding("speaker");
        CHECK(sp != nullptr && sp->verdict == Verdict::Ok);
        CHECK(sp->wiring == "DOUT 41" && sp->source == "known_map");
        // The microphone only found a signal that did not answer: ask a person.
        heard = false;
        pair.run(Mode::Quick);
        sp = pair.finding("speaker");
        CHECK(sp->verdict == Verdict::Unverified && sp->confirm_capability == "speaker.beep");
        // No microphone at all: same.
        mic_verdict = Verdict::NotFound;
        heard = true;
        pair.run(Mode::Quick);
        CHECK(pair.finding("speaker")->verdict == Verdict::Unverified);
        CHECK(pair.finding("nothing") == nullptr);
    }

    // A peripheral only a person can check.
    SelfCheck human;
    CHECK(human.add(make_human_confirmed_detector("speaker", "BCLK 42, WS 39, DOUT 41", "known_map",
                                                  "speaker.beep", "Did you hear one short beep?")));
    human.run(Mode::Quick);
    std::vector<Entry> hr = human.entries();
    CHECK(hr.size() == 1 && hr[0].klass == PeripheralClass::NoSignal);
    CHECK(hr[0].finding.verdict == Verdict::Unverified);  // never ok
    CHECK(human.report_json().find("\"confirm\":{\"capability\":\"speaker.beep\",\"question\":\"Did you hear one short beep?\"}") != std::string::npos);
    CHECK(human.report_json().find("\"class\":\"no_signal\"") != std::string::npos);
    // It does not degrade the device.
    Endpoint ep3("dev3", "t", "Dev3");
    attach_self_check(ep3, human);
    CHECK(ep3.handle("GET", "/status", "").body.find("\"status\":\"ok\"") != std::string::npos);
}

static void test_scan_pins() {
    using namespace dc_device::esp32;
    CHECK(safe_scan_pins("unknown_chip").empty());  // no table, no search
    CHECK(safe_scan_pins("esp32").empty());
    std::vector<int> pins = safe_scan_pins("esp32s3");
    CHECK(pins.size() == 25);
    for (int reserved : {0, 3, 19, 20, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 43, 44, 45, 46}) {
        for (int p : pins) CHECK(p != reserved);
    }
    CHECK(ordered_pairs(pins).size() == 25 * 24);
    auto two = ordered_pairs({4, 5});
    CHECK(two.size() == 2 && two[0].first == 4 && two[0].second == 5 && two[1].first == 5 && two[1].second == 4);
    CHECK(ordered_pairs({7}).empty());
    CHECK(!pin_claimed(4));
    claim_pin(4);
    CHECK(pin_claimed(4) && !pin_claimed(5));
    release_pin(4);
    CHECK(!pin_claimed(4));
}

// A fake I2C device: answers reads of the registers it was given, writes are recorded.
struct FakeI2c {
    uint8_t addr = 0;
    std::vector<std::pair<uint16_t, uint8_t>> regs;
    std::vector<std::pair<uint8_t, uint8_t>> writes;
    dc_device::esp32::I2cAccess access() {
        dc_device::esp32::I2cAccess io;
        io.read = [this](uint8_t a, uint16_t reg, int, uint8_t* out) {
            if (a != addr) return false;
            for (auto& r : regs) if (r.first == reg) { *out = r.second; return true; }
            return false;
        };
        io.write = [this](uint8_t a, uint8_t reg, uint8_t v) {
            if (a != addr) return false;
            writes.emplace_back(reg, v);
            return true;
        };
        return io;
    }
};

static void test_i2c_ids() {
    using namespace dc_device::esp32;
    FakeI2c cam{0x30, {{0x0A, 0x26}, {0x0B, 0x42}}, {}};
    I2cIdentity id = identify_i2c(0x30, cam.access());
    CHECK(id.confirmed && id.kind == "camera" && id.model == "ov2640");
    CHECK_EQ(id.evidence, "ov2640, id 0x26 0x42 at 0x30");
    CHECK(i2c_finding_name(id, 0x30) == "camera");
    CHECK(cam.writes.size() == 1 && cam.writes[0].first == 0xFF && cam.writes[0].second == 0x01);

    FakeI2c cam41{0x30, {{0x0A, 0x26}, {0x0B, 0x41}}, {}};
    CHECK(identify_i2c(0x30, cam41.access()).model == "ov2640");

    FakeI2c cam5{0x3C, {{0x300A, 0x56}, {0x300B, 0x40}}, {}};
    CHECK(identify_i2c(0x3C, cam5.access()).model == "ov5640");

    FakeI2c imu{0x68, {{0x75, 0x70}}, {}};  // MPU6500, not the first table row for 0x68
    id = identify_i2c(0x68, imu.access());
    CHECK(id.confirmed && id.model == "mpu6500" && i2c_finding_name(id, 0x68) == "imu@0x68");
    CHECK(imu.writes.empty());  // identifying an IMU writes nothing

    FakeI2c bme{0x76, {{0xD0, 0x60}}, {}};
    CHECK(identify_i2c(0x76, bme.access()).model == "bme280");

    FakeI2c codec{0x18, {{0xFD, 0x83}, {0xFE, 0x11}}, {}};
    CHECK(identify_i2c(0x18, codec.access()).kind == "audio_codec");

    // Something answers at 0x3C but it is not a camera: reported as a guess, not as fact.
    FakeI2c oled{0x3C, {}, {}};
    id = identify_i2c(0x3C, oled.access());
    CHECK(!id.confirmed && id.kind == "i2c");
    CHECK(id.evidence.find("chip not recognised") != std::string::npos);
    CHECK(id.evidence.find("a guess from the address") != std::string::npos);
    CHECK(i2c_finding_name(id, 0x3C) == "i2c@0x3c");

    // A wrong id value is not accepted.
    FakeI2c wrong{0x68, {{0x75, 0x99}}, {}};
    CHECK(!identify_i2c(0x68, wrong.access()).confirmed);

    // The OV2640 bank-select write is not done at an address the table does not list for it.
    FakeI2c other{0x31, {}, {}};
    CHECK(!identify_i2c(0x31, other.access()).confirmed);
    CHECK(other.writes.empty());

    size_t n = 0;
    const uint8_t* addrs = i2c_discovery_addresses(&n);
    CHECK(n > 10);
    bool has30 = false;
    for (size_t i = 0; i < n; ++i) { if (addrs[i] == 0x30) has30 = true; for (size_t j = i + 1; j < n; ++j) CHECK(addrs[i] != addrs[j]); }
    CHECK(has30);
}

static void test_audio_analysis() {
    using namespace dc_device::esp32;
    const int rate = 16000, n = 4800;
    std::vector<int32_t> v(n);

    // Nothing driving the line: every sample the same.
    for (auto& x : v) x = 0;
    MicStats m = analyze_mic(v.data(), v.size());
    CHECK(m.constant && !m.looks_like_mic);

    // A floating data pin: random bits in every position, low byte included.
    unsigned seed = 12345;
    for (auto& x : v) { seed = seed * 1664525u + 1013904223u; x = static_cast<int32_t>(seed); }
    m = analyze_mic(v.data(), v.size());
    CHECK(!m.constant && !m.looks_like_mic && m.low_byte_zero < 0.1);

    // A quiet microphone: small 24-bit values, left-justified, low byte zero.
    seed = 7;
    for (auto& x : v) { seed = seed * 1664525u + 1013904223u; x = static_cast<int32_t>(((seed >> 20) & 0x3FF) - 512) * 256; }
    m = analyze_mic(v.data(), v.size());
    CHECK(m.looks_like_mic && m.low_byte_zero > 0.99 && m.rms > 0 && m.rms < 0.01);

    // The same plus a 1 kHz tone: the tone shows up at 1 kHz and not at 2.3 kHz.
    const double kPi = 3.14159265358979323846;
    std::vector<int32_t> quiet = v;
    for (int i = 0; i < n; ++i) {
        double tone = 0.05 * std::sin(2 * kPi * 1000.0 * i / rate);
        v[i] = quiet[i] + static_cast<int32_t>(tone * 8388608.0) * 256;
    }
    double at1k = tone_power(v.data(), v.size(), rate, 1000.0);
    double off = tone_power(v.data(), v.size(), rate, 2300.0);
    double before = tone_power(quiet.data(), quiet.size(), rate, 1000.0);
    CHECK(at1k > 100 * before);
    CHECK(at1k > 100 * off);
    CHECK(analyze_mic(nullptr, 0).samples == 0);

    // The level test: a microphone passes, a floating pin and a stuck line do not.
    // The real board's samples had a non-zero low byte, which analyze_mic rejects.
    std::vector<int32_t> real(n);
    seed = 99;
    for (auto& x : real) { seed = seed * 1664525u + 1013904223u; x = (static_cast<int32_t>((seed >> 20) & 0xFFF) - 2048) * 1000; }
    CHECK(!analyze_mic(real.data(), real.size()).looks_like_mic);
    CHECK(plausible_mic_level(signal_stats(real.data(), real.size())));
    std::vector<int32_t> noise(n);
    seed = 5;
    for (auto& x : noise) { seed = seed * 1664525u + 1013904223u; x = static_cast<int32_t>(seed); }
    CHECK(!plausible_mic_level(signal_stats(noise.data(), noise.size())));
    std::vector<int32_t> stuck(n, 123456);
    CHECK(!plausible_mic_level(signal_stats(stuck.data(), stuck.size())));
    CHECK(!plausible_mic_level(signal_stats(nullptr, 0)));

    // A response to a known tone separates a microphone from a pin that just moves.
    std::vector<int32_t> heard = real;
    for (int i = 0; i < n; ++i) heard[i] = real[i] + static_cast<int32_t>(0.02 * std::sin(2 * kPi * 880.0 * i / rate) * 2147483647.0);
    CHECK(tone_gain(real.data(), n, heard.data(), n, rate, 880.0) > 50);
    // The same line read twice with no tone: no gain.
    std::vector<int32_t> again(n);
    seed = 1234;
    for (auto& x : again) { seed = seed * 1664525u + 1013904223u; x = (static_cast<int32_t>((seed >> 20) & 0xFFF) - 2048) * 1000; }
    CHECK(tone_gain(real.data(), n, again.data(), n, rate, 880.0) < 3);
}

// ---- USB setup protocol (docs/usb-setup-protocol-v0.1.md) ----------------------

struct SetupRig {
    std::vector<std::string> out;  // every line the device wrote
    WifiState state = WifiState::Unconfigured;
    std::string ip;
    int forgets = 0;
    bool forget_ok = true;
    // `wifi` answers at once with this, unless `defer` is set: then `finish` runs it later.
    WifiOutcome outcome;
    bool defer = false;
    std::function<void(const WifiOutcome&)> finish;
    std::function<void(const std::string&)> progress_cb;
    std::string seen_ssid, seen_password;
    int wifi_calls = 0;
    bool is_paired = false;
    bool issue_ok = true;
    int tokens_issued = 0;
    std::string current_token;
    SetupProtocol proto;

    static SetupInfo info() {
        SetupInfo i;
        i.device_id = "camera_board_01";
        i.type = "camera_board";
        i.model = "ESP32-S3 Camera Board";
        i.firmware_version = "0.1.0+abc1234";
        i.serial = "B8F862E96718";
        return i;
    }
    SetupBackend backend() {
        SetupBackend b;
        b.state = [this] { return state; };
        b.ip = [this] { return ip; };
        b.port = 8787;
        b.start_wifi = [this](const std::string& ssid, const std::string& password,
                              std::function<void(const std::string&)> progress,
                              std::function<void(const WifiOutcome&)> done) {
            ++wifi_calls;
            seen_ssid = ssid;
            seen_password = password;
            progress("connecting");
            progress_cb = progress;
            if (defer) {
                finish = done;
            } else {
                done(outcome);
            }
        };
        b.forget_wifi = [this] {
            ++forgets;
            return forget_ok;
        };
        b.paired = [this] { return is_paired; };
        b.issue_token = [this](std::string& token) {
            if (!issue_ok) return false;
            ++tokens_issued;
            current_token = "tok-" + std::to_string(tokens_issued) + "-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
            is_paired = true;
            token = current_token;
            return true;
        };
        b.unpair = [this] {
            is_paired = false;
            current_token.clear();
            return true;
        };
        return b;
    }
    SetupRig() : proto(info(), backend(), [this](const std::string& line) { out.push_back(line); }) {}
    void say(const std::string& text) { proto.feed(text.data(), text.size()); }
    void line(const std::string& json) { say("DCSETUP " + json + "\n"); }
    std::string last() const { return out.empty() ? "" : out.back(); }
};

static std::string hex_of(const std::array<uint8_t, 32>& d) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : d) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

static void test_sha256() {
    auto h = [](const std::string& s) { return hex_of(sha256(s.data(), s.size())); };
    CHECK_EQ(h(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(h("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(h(std::string(1000000, 'a')),
             "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Lengths around the 55/56/64-byte padding boundaries.
    CHECK_EQ(h(std::string(55, 'a')), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK_EQ(h(std::string(56, 'a')), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK_EQ(h(std::string(64, 'a')), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

struct AccessRig {
    std::array<uint8_t, 32> stored{};
    bool has_stored = false;
    bool save_ok = true;
    bool load_error = false;  // storage could not be read at all
    double now = 1000;
    uint8_t counter = 0;
    AccessStorage storage() {
        AccessStorage st;
        st.load = [this](std::array<uint8_t, 32>& h) {
            if (load_error) return LoadResult::Error;
            if (!has_stored) return LoadResult::NotFound;
            h = stored;
            return LoadResult::Found;
        };
        st.save = [this](const std::array<uint8_t, 32>& h) {
            if (!save_ok) return false;
            stored = h;
            has_stored = true;
            return true;
        };
        st.erase = [this]() {
            if (!save_ok) return false;
            has_stored = false;
            return true;
        };
        return st;
    }
    AccessControl make(bool enforce) {
        return AccessControl(enforce, storage(),
                             [this](uint8_t* out, size_t n) {
                                 for (size_t i = 0; i < n; ++i) out[i] = uint8_t(counter + i * 7);
                                 counter += 31;
                             },
                             [this]() { return now; });
    }
};

static void test_access_control() {
    // token_encode: URL-safe, no padding, 43 characters for 32 bytes.
    {
        uint8_t bytes[32];
        for (int i = 0; i < 32; ++i) bytes[i] = uint8_t(0xF8 + (i % 8));
        std::string t = token_encode(bytes, 32);
        CHECK(t.size() == 43);
        for (char c : t) CHECK(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_');
        CHECK_EQ(token_encode(reinterpret_cast<const uint8_t*>("Man"), 3), "TWFu");
        CHECK_EQ(token_encode(reinterpret_cast<const uint8_t*>("Ma"), 2), "TWE");
    }
    // Unpaired: release refuses, development allows.
    {
        AccessRig rig;
        CHECK(rig.make(true).check("") == AuthResult::Unpaired);
        CHECK(rig.make(true).required() && !rig.make(true).paired());
        CHECK(rig.make(false).check("") == AuthResult::Allowed);
        CHECK(rig.make(false).check("Bearer whatever") == AuthResult::Allowed);
        CHECK(!rig.make(false).required());
    }
    // Paired: only the exact token passes.
    {
        AccessRig rig;
        AccessControl ac = rig.make(true);
        std::string t, t2;
        CHECK(ac.issue(t));
        CHECK(ac.paired() && ac.required());
        CHECK(ac.check("Bearer " + t) == AuthResult::Allowed);
        const std::string wrong[] = {"",           "Bearer ",      "Bearer",        "bearer " + t,
                                     "Basic " + t, "Bearer " + t + "x", "Bearer " + t.substr(0, 42),
                                     "Bearer  " + t, t,            "Bearer " + std::string(500, 'A')};
        for (const auto& h : wrong) {
            AuthResult r = ac.check(h);
            CHECK(r == AuthResult::Unauthorized);
        }
        // Re-keying: the first token dies at once.
        CHECK(ac.issue(t2));
        CHECK(t != t2);
        CHECK(ac.check("Bearer " + t) == AuthResult::Unauthorized);
        CHECK(ac.check("Bearer " + t2) == AuthResult::Allowed);
    }
    // Lockout: ten wrong tokens in a row, then 30 s of refusing everything.
    {
        AccessRig rig;
        AccessControl ac = rig.make(true);
        std::string t;
        CHECK(ac.issue(t));
        for (int i = 0; i < 9; ++i) CHECK(ac.check("Bearer nope") == AuthResult::Unauthorized);
        CHECK(ac.check("Bearer " + t) == AuthResult::Allowed);  // a correct one resets the run
        CHECK(ac.consecutive_failures() == 0);
        for (int i = 0; i < 10; ++i) CHECK(ac.check("Bearer nope") == AuthResult::Unauthorized);
        CHECK(ac.check("Bearer " + t) == AuthResult::LockedOut);
        rig.now += 29.9;
        CHECK(ac.check("Bearer " + t) == AuthResult::LockedOut);
        CHECK(ac.diagnostics_json().find("\"locked_out\":true") != std::string::npos);
        rig.now += 0.3;
        CHECK(ac.check("Bearer " + t) == AuthResult::Allowed);
        CHECK(ac.consecutive_failures() == 0);
        // No header at all is not a guess and does not count.
        for (int i = 0; i < 30; ++i) CHECK(ac.check("") == AuthResult::Unauthorized);
        CHECK(ac.check("Bearer " + t) == AuthResult::Allowed);
    }
    // Storage: pairing survives a restart; a failing store changes nothing.
    {
        AccessRig rig;
        std::string t;
        {
            AccessControl ac = rig.make(true);
            CHECK(ac.issue(t));
        }
        AccessControl again = rig.make(true);
        CHECK(again.paired());
        CHECK(again.check("Bearer " + t) == AuthResult::Allowed);
        rig.save_ok = false;
        std::string t2;
        CHECK(!again.issue(t2));
        CHECK(again.check("Bearer " + t) == AuthResult::Allowed);
        CHECK(!again.revoke());
        CHECK(again.paired());
        rig.save_ok = true;
        CHECK(again.revoke());
        CHECK(!again.paired());
        CHECK(again.check("Bearer " + t) == AuthResult::Unpaired);
        CHECK(again.revoke());  // unpaired already: still ok
        // The failure counter is RAM only: a new instance starts at 0.
        CHECK(rig.make(true).consecutive_failures() == 0);
    }
    // A storage read that fails is not "unpaired": the device fails closed, even a
    // development one, and `token` brings it back.
    {
        AccessRig rig;
        rig.load_error = true;
        AccessControl ac = rig.make(false);
        CHECK(ac.paired());
        CHECK(ac.check("") == AuthResult::Unauthorized);
        CHECK(ac.check("Bearer anything") == AuthResult::Unauthorized);
        std::string t;
        CHECK(ac.issue(t));
        CHECK(ac.check("Bearer " + t) == AuthResult::Allowed);
        rig.load_error = true;
        AccessControl again = rig.make(false);
        CHECK(again.revoke());  // unpair works on a board whose hash cannot be read
        CHECK(!again.paired());
        CHECK(!rig.has_stored);
    }
    // Diagnostics never hold the token or its hash.
    {
        AccessRig rig;
        AccessControl ac = rig.make(true);
        std::string t;
        CHECK(ac.issue(t));
        ac.check("Bearer nope");
        std::string d = ac.diagnostics_json();
        CHECK_EQ(d, R"({"paired":true,"consecutive_failures":1,"locked_out":false})");
        CHECK(d.find(t) == std::string::npos);
    }
}

static void test_endpoint_auth() {
    AccessRig rig;
    AccessControl ac = rig.make(true);
    Endpoint ep = make_endpoint();
    ep.set_access_control(&ac);
    const char* invoke_body = R"({"capability":"led.set","arguments":{"on":true}})";

    // Release and unpaired: protected calls refused, nothing runs.
    g_led_on = false;
    Response r = ep.handle("POST", "/invoke", invoke_body);
    CHECK(r.status == 401);
    CHECK_EQ(r.www_authenticate, "Bearer");
    CHECK(r.body.find("\"error\":\"Unpaired\"") != std::string::npos);
    CHECK(!g_led_on);

    std::string t;
    CHECK(ac.issue(t));
    const std::string bearer = "Bearer " + t;
    // Every protected route: refused without a header, with a wrong one; passes with the right one.
    struct Route {
        const char* method;
        const char* path;
        const char* body;
    };
    const Route routes[] = {{"POST", "/invoke", invoke_body}, {"POST", "/self_test", ""}, {"GET", "/diagnostics", ""}};
    for (const Route& route : routes) {
        g_led_on = false;
        Response none = ep.handle(route.method, route.path, route.body);
        CHECK(none.status == 401);
        CHECK_EQ(none.www_authenticate, "Bearer");
        CHECK(none.body.find("\"ok\":false,\"error\":\"Unauthorized\"") != std::string::npos);
        CHECK(none.body.find(t) == std::string::npos);
        Response bad = ep.handle(route.method, route.path, route.body, "Bearer nope");
        CHECK(bad.status == 401);
        CHECK(!g_led_on);
        Response good = ep.handle(route.method, route.path, route.body, bearer);
        CHECK(good.status == 200);
        CHECK(good.www_authenticate.empty());
        if (std::string(route.path) == "/invoke") CHECK(g_led_on);
    }

    // Open routes never look at the header: a wrong token there is fine and counts for nothing.
    for (int i = 0; i < 20; ++i) {
        for (const char* path : {"/health", "/identity", "/manifest", "/status"}) {
            CHECK(ep.handle("GET", path, "", "Bearer nope").status == 200);
        }
    }
    CHECK(ac.consecutive_failures() == 0);
    CHECK(ep.handle("POST", "/invoke", invoke_body, bearer).status == 200);

    // Unknown paths and wrong methods stay 404, header or not.
    CHECK(ep.handle("GET", "/nothing", "").status == 404);
    CHECK(ep.handle("GET", "/invoke", "").status == 404);
    CHECK(ep.handle("GET", "/self_test", "").status == 404);

    // Locked out: 401 even with the right token.
    for (int i = 0; i < 10; ++i) ep.handle("POST", "/invoke", invoke_body, "Bearer nope");
    Response locked = ep.handle("POST", "/invoke", invoke_body, bearer);
    CHECK(locked.status == 401);
    CHECK(locked.body.find("\"error\":\"Unauthorized\"") != std::string::npos);
    rig.now += 31;
    CHECK(ep.handle("POST", "/invoke", invoke_body, bearer).status == 200);

    // /identity says which state the device is in.
    CHECK(ep.identity().find(R"("auth":{"required":true,"paired":true})") != std::string::npos);
    CHECK(ac.revoke());
    CHECK(ep.identity().find(R"("auth":{"required":true,"paired":false})") != std::string::npos);

    // Development build, unpaired: open, and /identity says so.
    AccessRig dev_rig;
    AccessControl dev = dev_rig.make(false);
    Endpoint dev_ep = make_endpoint();
    dev_ep.set_access_control(&dev);
    CHECK(dev_ep.handle("POST", "/invoke", invoke_body).status == 200);
    CHECK(dev_ep.identity().find(R"("auth":{"required":false,"paired":false})") != std::string::npos);
    std::string dt;
    CHECK(dev.issue(dt));
    CHECK(dev_ep.handle("POST", "/invoke", invoke_body).status == 401);  // paired dev boards enforce
    CHECK(dev_ep.identity().find(R"("auth":{"required":true,"paired":true})") != std::string::npos);

    // Without an AccessControl nothing changes (existing behaviour and identity bytes).
    Endpoint plain = make_endpoint();
    CHECK(plain.handle("POST", "/invoke", invoke_body).status == 200);
    CHECK(plain.identity().find("\"auth\"") == std::string::npos);
}

static void test_setup_protocol() {
    // hello: no secret, recognises the device, reports the state.
    {
        SetupRig r;
        r.line(R"({"v":1,"id":7,"cmd":"hello"})");
        CHECK_EQ(r.last(),
                 R"(DCSETUP {"v":1,"id":7,"ok":true,"setup_protocol":1,"device_id":"camera_board_01",)"
                 R"("type":"camera_board","model":"ESP32-S3 Camera Board","firmware_version":"0.1.0+abc1234",)"
                 R"("serial":"B8F862E96718","paired":false,"state":"unconfigured"})");
        r.state = WifiState::Connected;
        r.ip = "192.168.1.20";
        r.line(R"({"v":1,"id":8,"cmd":"hello"})");
        CHECK(r.last().find(R"("state":"connected","port":8787,"ip":"192.168.1.20")") != std::string::npos);
        for (auto st : {WifiState::Connecting, WifiState::Offline}) {
            r.state = st;
            r.line(R"({"v":1,"id":9,"cmd":"hello"})");
            CHECK(r.last().find(std::string("\"state\":\"") + wifi_state_name(st) + "\"}") != std::string::npos);
        }
    }
    // token, unpair, hello.paired.
    {
        SetupRig r;
        r.line(R"({"v":1,"id":20,"cmd":"token"})");
        CHECK_EQ(r.last(), "DCSETUP {\"v\":1,\"id\":20,\"ok\":true,\"token\":\"" + r.current_token + "\"}");
        std::string first = r.current_token;
        r.line(R"({"v":1,"id":21,"cmd":"hello"})");
        CHECK(r.last().find("\"serial\":\"B8F862E96718\",\"paired\":true,") != std::string::npos);
        CHECK(r.last().find(first) == std::string::npos);
        r.line(R"({"v":1,"id":22,"cmd":"token"})");  // re-keying replaces it
        CHECK(r.current_token != first);
        CHECK(r.last().find(r.current_token) != std::string::npos);
        // The token is in exactly the two token replies and nowhere else.
        r.line(R"({"v":1,"id":23,"cmd":"forget_wifi"})");
        r.line(R"({"v":1,"id":24,"cmd":"nope"})");
        int with_token = 0;
        for (const auto& line : r.out) {
            if (line.find("tok-") != std::string::npos) ++with_token;
        }
        CHECK(with_token == 2);
        r.line(R"({"v":1,"id":25,"cmd":"hello"})");  // forget_wifi does not unpair
        CHECK(r.last().find("\"paired\":true") != std::string::npos);
        r.line(R"({"v":1,"id":26,"cmd":"unpair"})");
        CHECK_EQ(r.last(), "DCSETUP {\"v\":1,\"id\":26,\"ok\":true,\"paired\":false}");
        r.line(R"({"v":1,"id":27,"cmd":"unpair"})");  // already unpaired: still ok
        CHECK(r.last().find("\"ok\":true") != std::string::npos);
        r.line(R"({"v":1,"id":28,"cmd":"hello"})");
        CHECK(r.last().find("\"paired\":false") != std::string::npos);
    }
    {
        SetupRig r;
        r.issue_ok = false;  // storage failed
        r.line(R"({"v":1,"id":30,"cmd":"token"})");
        CHECK(r.last().find("\"ok\":false") != std::string::npos);
        CHECK(r.last().find("\"code\":\"internal\"") != std::string::npos);
        CHECK(!r.is_paired);
        SetupBackend none;  // a device with no access control at all
        std::vector<std::string> out;
        SetupProtocol p(SetupRig::info(), none, [&](const std::string& l) { out.push_back(l); });
        std::string text = "DCSETUP {\"v\":1,\"id\":31,\"cmd\":\"token\"}\nDCSETUP {\"v\":1,\"id\":32,\"cmd\":\"unpair\"}\n";
        p.feed(text.data(), text.size());
        CHECK(out.size() == 2);
        CHECK(out[0].find("\"code\":\"internal\"") != std::string::npos);
        CHECK(out[1].find("\"code\":\"internal\"") != std::string::npos);
    }
    // Framing: log noise and lines without the prefix are ignored, \r\n works, a
    // line can arrive in pieces, an empty line is ignored.
    {
        SetupRig r;
        r.say("I (123) boot: starting\n\n");
        r.say("garbage DCSETUP {\"v\":1,\"id\":1,\"cmd\":\"hello\"}\n");  // prefix not at the start
        r.say("DCSETUP{\"v\":1,\"id\":1,\"cmd\":\"hello\"}\n");           // no space
        r.say("dcsetup {\"v\":1,\"id\":1,\"cmd\":\"hello\"}\n");          // case matters
        CHECK(r.out.empty());
        r.say("DCSETUP {\"v\":1,\"id\":2,");
        CHECK(r.out.empty());
        r.say("\"cmd\":\"hello\"}\r\n");
        CHECK(r.out.size() == 1 && r.last().find("\"id\":2,\"ok\":true") != std::string::npos);
        // A partial log line before the protocol line is cut off by the sender's own \n.
        r.say("W (5) wifi: partial");
        r.say("\nDCSETUP {\"v\":1,\"id\":3,\"cmd\":\"hello\"}\n");
        CHECK(r.out.size() == 2 && r.last().find("\"id\":3,\"ok\":true") != std::string::npos);
    }
    // Errors that apply to any command.
    {
        SetupRig r;
        r.line("not json");
        CHECK_EQ(r.last(),
                 R"(DCSETUP {"v":1,"id":null,"ok":false,"error":{"code":"bad_request","message":"The line is not a JSON object."}})");
        r.line("[1,2]");
        CHECK(r.last().find("\"id\":null") != std::string::npos && r.last().find("bad_request") != std::string::npos);
        r.line(R"({"v":1,"cmd":"hello"})");  // no id
        CHECK(r.last().find("\"id\":null") != std::string::npos && r.last().find("bad_request") != std::string::npos);
        r.line(R"({"v":1,"id":"x","cmd":"hello"})");  // id not an integer
        CHECK(r.last().find("\"id\":null") != std::string::npos);
        r.line(R"({"v":1,"id":1.5,"cmd":"hello"})");
        CHECK(r.last().find("\"id\":null") != std::string::npos);
        r.line(R"({"id":4,"cmd":"hello"})");  // no v
        CHECK(r.last().find("\"id\":4") != std::string::npos && r.last().find("bad_request") != std::string::npos);
        r.line(R"({"v":2,"id":5,"cmd":"hello"})");
        CHECK(r.last().find("unsupported_version") != std::string::npos &&
              r.last().find("\"supported\":[1]") != std::string::npos && r.last().find("\"id\":5") != std::string::npos);
        r.line(R"({"v":1,"id":6})");  // no cmd
        CHECK(r.last().find("bad_request") != std::string::npos && r.last().find("\"id\":6") != std::string::npos);
        r.line(R"({"v":1,"id":6,"cmd":"reboot"})");
        CHECK(r.last().find("unknown_command") != std::string::npos);
        // Unknown fields are ignored.
        r.line(R"({"v":1,"id":9,"cmd":"hello","future":true})");
        CHECK(r.last().find("\"ok\":true") != std::string::npos);
    }
    // A line over 512 bytes is discarded to its end, answered once, and the next line works.
    {
        SetupRig r;
        std::string big = std::string("DCSETUP {\"v\":1,\"id\":1,\"cmd\":\"hello\",\"pad\":\"") + std::string(600, 'a') + "\"}\n";
        r.say(big);
        CHECK(r.out.size() == 1 && r.last().find("line_too_long") != std::string::npos &&
              r.last().find("\"id\":null") != std::string::npos);
        r.line(R"({"v":1,"id":2,"cmd":"hello"})");
        CHECK(r.out.size() == 2 && r.last().find("\"id\":2,\"ok\":true") != std::string::npos);
        // A long line that is not ours is just noise: no answer.
        r.say(std::string(2000, 'x') + "\n");
        CHECK(r.out.size() == 2);
        // The largest line allowed still works: 512 bytes including the prefix.
        std::string head = R"({"v":1,"id":3,"cmd":"hello","pad":")";
        std::string tail = R"("})";
        std::string pad(512 - 8 - head.size() - tail.size(), 'b');
        r.line(head + pad + tail);
        CHECK(r.out.size() == 3 && r.last().find("\"id\":3,\"ok\":true") != std::string::npos);
        r.line(head + pad + "b" + tail);  // one byte more
        CHECK(r.out.size() == 4 && r.last().find("line_too_long") != std::string::npos);
    }
    // wifi: validation first, nothing reaches the backend.
    {
        SetupRig r;
        r.line(R"({"v":1,"id":1,"cmd":"wifi","ssid":"","password":"correct horse"})");
        CHECK(r.last().find("invalid_ssid") != std::string::npos);
        r.line(R"({"v":1,"id":1,"cmd":"wifi","ssid":")" + std::string(33, 's') + R"(","password":"correct horse"})");
        CHECK(r.last().find("invalid_ssid") != std::string::npos);
        r.line(R"({"v":1,"id":1,"cmd":"wifi","ssid":"net","password":"short"})");
        CHECK(r.last().find("invalid_password") != std::string::npos);
        r.line(R"({"v":1,"id":1,"cmd":"wifi","ssid":"net","password":")" + std::string(64, 'p') + R"("})");
        CHECK(r.last().find("invalid_password") != std::string::npos);
        r.line(R"({"v":1,"id":1,"cmd":"wifi","ssid":"net"})");  // password missing
        CHECK(r.last().find("bad_request") != std::string::npos);
        r.line(R"({"v":1,"id":1,"cmd":"wifi","ssid":5,"password":""})");
        CHECK(r.last().find("bad_request") != std::string::npos);
        CHECK(r.wifi_calls == 0);
        // Edges that are valid: 32-byte name, 8 and 63 character passwords, open network.
        for (std::string pw : {std::string(8, 'p'), std::string(63, 'p'), std::string()}) {
            r.outcome.ok = true;
            r.outcome.ip = "10.0.0.2";
            r.line(R"({"v":1,"id":2,"cmd":"wifi","ssid":")" + std::string(32, 's') + R"(","password":")" + pw + R"("})");
            CHECK(r.last().find("\"ok\":true") != std::string::npos);
        }
        CHECK(r.wifi_calls == 3);
    }
    // wifi success: progress event, then the answer; the password is never echoed.
    {
        SetupRig r;
        r.outcome.ok = true;
        r.outcome.ip = "192.168.1.20";
        r.line(R"({"v":1,"id":2,"cmd":"wifi","ssid":"SchoolNet","password":"correct horse"})");
        CHECK(r.out.size() == 2);
        CHECK_EQ(r.out[0], R"(DCSETUP {"v":1,"id":2,"event":"progress","stage":"connecting"})");
        CHECK_EQ(r.out[1], R"(DCSETUP {"v":1,"id":2,"ok":true,"state":"connected","ip":"192.168.1.20","port":8787})");
        CHECK(r.seen_ssid == "SchoolNet" && r.seen_password == "correct horse");
        for (const std::string& line : r.out) CHECK(line.find("correct horse") == std::string::npos);
        // Characters that need escaping in JSON reach the backend intact.
        r.line(R"({"v":1,"id":3,"cmd":"wifi","ssid":"café \"x\"","password":"p\\w\"rd1234"})");
        CHECK(r.seen_ssid == "caf\xC3\xA9 \"x\"" && r.seen_password == "p\\w\"rd1234");
    }
    // wifi failures carry the backend's code and are not mistaken for success.
    {
        SetupRig r;
        for (const char* code : {"wifi_not_found", "wifi_auth_failed", "wifi_no_address", "wifi_timeout"}) {
            r.outcome.ok = false;
            r.outcome.code = code;
            r.outcome.message = "nope";
            r.line(R"({"v":1,"id":4,"cmd":"wifi","ssid":"net","password":"correct horse"})");
            CHECK(r.last().find(std::string("\"code\":\"") + code + "\"") != std::string::npos);
            CHECK(r.last().find("\"ok\":false") != std::string::npos && r.last().find("connected") == std::string::npos);
        }
        r.outcome.code = "";  // a backend that forgot the code: internal, never silent
        r.line(R"({"v":1,"id":4,"cmd":"wifi","ssid":"net","password":"correct horse"})");
        CHECK(r.last().find("\"code\":\"internal\"") != std::string::npos);
    }
    // While a wifi command runs: hello still answers, a second wifi or forget is busy,
    // and the slot frees when it finishes.
    {
        SetupRig r;
        r.defer = true;
        r.state = WifiState::Connecting;
        r.line(R"({"v":1,"id":1,"cmd":"wifi","ssid":"net","password":"correct horse"})");
        CHECK(r.out.size() == 1);  // the progress event; no answer yet
        r.line(R"({"v":1,"id":2,"cmd":"hello"})");
        CHECK(r.last().find("\"id\":2,\"ok\":true") != std::string::npos &&
              r.last().find("\"state\":\"connecting\"") != std::string::npos);
        r.line(R"({"v":1,"id":3,"cmd":"wifi","ssid":"net2","password":"correct horse"})");
        CHECK(r.last().find("\"code\":\"busy\"") != std::string::npos && r.last().find("\"id\":3") != std::string::npos);
        r.line(R"({"v":1,"id":4,"cmd":"forget_wifi"})");
        CHECK(r.last().find("\"code\":\"busy\"") != std::string::npos);
        CHECK(r.wifi_calls == 1 && r.forgets == 0);
        r.progress_cb("getting_address");
        CHECK(r.last().find("\"stage\":\"getting_address\"") != std::string::npos);
        WifiOutcome ok;
        ok.ok = true;
        ok.ip = "10.1.1.9";
        r.finish(ok);
        CHECK(r.last().find("\"id\":1,\"ok\":true,\"state\":\"connected\"") != std::string::npos);
        r.defer = false;
        r.outcome = ok;
        r.line(R"({"v":1,"id":5,"cmd":"wifi","ssid":"net","password":"correct horse"})");
        CHECK(r.last().find("\"id\":5,\"ok\":true") != std::string::npos);
    }
    // forget_wifi.
    {
        SetupRig r;
        r.line(R"({"v":1,"id":1,"cmd":"forget_wifi"})");
        CHECK_EQ(r.last(), R"(DCSETUP {"v":1,"id":1,"ok":true,"state":"unconfigured"})");
        CHECK(r.forgets == 1);
        r.forget_ok = false;
        r.line(R"({"v":1,"id":2,"cmd":"forget_wifi"})");
        CHECK(r.last().find("\"code\":\"internal\"") != std::string::npos);
    }
    CHECK_EQ(wifi_state_name(WifiState::Offline), "offline");
}

int main() {
    test_identity();
    test_health_registration();
    test_health_routes();
    test_health_states();
    test_json();
    test_registration();
    test_routes();
    test_invoke();
    test_rate_limit();
    test_safety_kernel();
    test_chip_facts();
    test_selfcheck();
    test_scan_pins();
    test_i2c_ids();
    test_audio_analysis();
    test_setup_protocol();
    test_sha256();
    test_access_control();
    test_endpoint_auth();
    if (g_failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
