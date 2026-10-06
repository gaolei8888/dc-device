#include "dc_device_json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dc_device::json {

const Value* Value::find(const std::string& key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            return &items[i];
        }
    }
    return nullptr;
}

namespace {

constexpr int kMaxDepth = 32;

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool run(Value& out, std::string& error) {
        skip_ws();
        if (!value(out, 0)) {
            error = error_;
            return false;
        }
        skip_ws();
        if (pos_ != s_.size()) {
            fail("unexpected trailing characters");
            error = error_;
            return false;
        }
        return true;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
    std::string error_;

    bool fail(const char* message) {
        if (error_.empty()) {
            error_ = std::string(message) + " at offset " + std::to_string(pos_);
        }
        return false;
    }

    void skip_ws() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(const char* word) {
        size_t n = 0;
        while (word[n] != '\0') {
            ++n;
        }
        if (s_.compare(pos_, n, word) != 0) {
            return fail("invalid literal");
        }
        pos_ += n;
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        if (pos_ >= s_.size()) {
            return fail("unexpected end of input");
        }
        switch (s_[pos_]) {
            case '{':
                return object(out, depth);
            case '[':
                return array(out, depth);
            case '"':
                out.type = Value::Type::String;
                return string(out.string);
            case 't':
                out.type = Value::Type::Bool;
                out.boolean = true;
                return literal("true");
            case 'f':
                out.type = Value::Type::Bool;
                out.boolean = false;
                return literal("false");
            case 'n':
                out.type = Value::Type::Null;
                return literal("null");
            default:
                return number_value(out);
        }
    }

    bool object(Value& out, int depth) {
        out.type = Value::Type::Object;
        ++pos_;  // '{'
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') {
                return fail("expected object key");
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                return fail("expected ':'");
            }
            ++pos_;
            skip_ws();
            Value member;
            if (!value(member, depth + 1)) {
                return false;
            }
            out.keys.push_back(std::move(key));
            out.items.push_back(std::move(member));
            skip_ws();
            if (pos_ >= s_.size()) {
                return fail("unterminated object");
            }
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Value& out, int depth) {
        out.type = Value::Type::Array;
        ++pos_;  // '['
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            Value element;
            if (!value(element, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(element));
            skip_ws();
            if (pos_ >= s_.size()) {
                return fail("unterminated array");
            }
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(unsigned& out) {
        if (pos_ + 4 > s_.size()) {
            return fail("truncated \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_ + i];
            unsigned digit;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
            } else {
                return fail("invalid \\u escape");
            }
            out = (out << 4) | digit;
        }
        pos_ += 4;
        return true;
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= s_.size()) {
                return fail("unterminated string");
            }
            unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out += static_cast<char>(c);
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= s_.size()) {
                return fail("unterminated escape");
            }
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned low;
                        if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
                            return fail("unpaired surrogate");
                        }
                        pos_ += 2;
                        if (!hex4(low)) {
                            return false;
                        }
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return fail("invalid surrogate pair");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid escape");
            }
        }
    }

    bool digits() {
        size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            ++pos_;
        }
        return pos_ > start;
    }

    bool number_value(Value& out) {
        size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ < s_.size() && s_[pos_] == '0') {
            ++pos_;
        } else if (!digits()) {
            pos_ = start;
            return fail("invalid value");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (!digits()) {
                return fail("invalid number");
            }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
            if (!digits()) {
                return fail("invalid number");
            }
        }
        out.type = Value::Type::Number;
        out.number = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string& error) {
    out = Value{};
    error.clear();
    Parser parser(text);
    return parser.run(out, error);
}

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

std::string number(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    char buf[32];
    if (value == 0.0) {
        return "0";
    }
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        std::snprintf(buf, sizeof(buf), "%.0f", value);
    } else {
        std::snprintf(buf, sizeof(buf), "%.6g", value);
    }
    return buf;
}

}  // namespace dc_device::json
