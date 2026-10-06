#pragma once

#include <string>
#include <vector>

namespace dc_device::json {

// Minimal JSON value. Exception-free so it works with ESP-IDF's default
// build settings (C++ exceptions disabled).
//
// Objects keep their members in `keys` / `items` (parallel vectors) so that
// Value can contain itself without relying on incomplete-type containers.
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<std::string> keys;  // Object only
    std::vector<Value> items;       // Array elements, or Object member values

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    // Object member lookup. Returns nullptr when absent or not an object.
    const Value* find(const std::string& key) const;
};

// Strict RFC 8259 parse of a complete document. On failure returns false and
// sets `error`.
bool parse(const std::string& text, Value& out, std::string& error);

// Returns `text` as a quoted, escaped JSON string literal.
std::string quote(const std::string& text);

// Formats a number for JSON output. Integral values print without a
// fraction; non-finite values print as null.
std::string number(double value);

}  // namespace dc_device::json
