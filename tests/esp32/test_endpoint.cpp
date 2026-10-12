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
    ep.handle("POST", "/invoke", R"({"capability":"stop"})");
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
    if (g_failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
