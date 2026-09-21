#pragma once

#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace eda {

// Dependency-free JSON syntax reader for thermal input. Unlike key searches,
// this checks the entire document, nesting, duplicate keys and number grammar.
struct JsonValue {
    enum class Kind { OBJECT, ARRAY, STRING, NUMBER, OTHER };
    Kind kind{Kind::OTHER};
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
    double number{0};
    const JsonValue& at(const std::string& key) const {
        if (kind != Kind::OBJECT || !object.contains(key))
            throw std::runtime_error("missing JSON object key: " + key);
        return object.at(key);
    }
    double as_number() const {
        if (kind != Kind::NUMBER) throw std::runtime_error("expected JSON number");
        return number;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& source) : source_(source) {}
    JsonValue read() {
        auto value = read_value(0);
        space();
        if (pos_ != source_.size()) fail();
        return value;
    }
private:
    const std::string& source_;
    size_t pos_{0};
    [[noreturn]] void fail() const {
        throw std::runtime_error("invalid JSON at byte " + std::to_string(pos_));
    }
    char peek() const { return pos_ < source_.size() ? source_[pos_] : '\0'; }
    void space() {
        while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') ++pos_;
    }
    bool accept(char c) {
        space();
        if (peek() != c) return false;
        ++pos_; return true;
    }
    void expect(char c) { if (!accept(c)) fail(); }
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    unsigned hex4() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = peek();
            unsigned d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else fail();
            value = value * 16 + d; ++pos_;
        }
        return value;
    }
    std::string string() {
        expect('"');
        std::string result;
        while (peek() != '"') {
            unsigned char c = static_cast<unsigned char>(peek());
            if (c < 0x20) fail();
            ++pos_;
            if (c != '\\') { result += static_cast<char>(c); continue; }
            c = static_cast<unsigned char>(peek()); ++pos_;
            if (c == '"' || c == '/' || c == '\\') result += static_cast<char>(c);
            else if (c == 'b') result += '\b';
            else if (c == 'f') result += '\f';
            else if (c == 'n') result += '\n';
            else if (c == 'r') result += '\r';
            else if (c == 't') result += '\t';
            else if (c == 'u') {
                unsigned code = hex4();
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (peek() != '\\') fail();
                    ++pos_;
                    if (peek() != 'u') fail();
                    ++pos_;
                    const unsigned low = hex4();
                    if (low < 0xDC00 || low > 0xDFFF) fail();
                    code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
                } else if (code >= 0xDC00 && code <= 0xDFFF) fail();
                if (code <= 0x7F) result += static_cast<char>(code);
                else {
                    if (code > 0xFFFF) result += static_cast<char>(0xF0 | (code >> 18));
                    if (code > 0x7FF) result += static_cast<char>((code > 0xFFFF ? 0x80 : 0xE0) | ((code >> 12) & 0x3F));
                    result += static_cast<char>((code > 0x7FF ? 0x80 : 0xC0) | ((code >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                }
            } else fail();
        }
        ++pos_; return result;
    }
    JsonValue read_value(int depth) {
        if (depth > 64) fail();
        space();
        JsonValue value;
        if (accept('{')) {
            value.kind = JsonValue::Kind::OBJECT;
            if (accept('}')) return value;
            do {
                const auto key = string(); expect(':');
                if (!value.object.emplace(key, read_value(depth + 1)).second) fail();
                if (accept('}')) return value;
            } while (accept(','));
            fail();
        } else if (accept('[')) {
            value.kind = JsonValue::Kind::ARRAY;
            if (accept(']')) return value;
            do {
                value.array.push_back(read_value(depth + 1));
                if (accept(']')) return value;
            } while (accept(','));
            fail();
        } else if (peek() == '"') {
            string(); value.kind = JsonValue::Kind::STRING; return value;
        } else if (peek() == '-' || digit(peek())) {
            const size_t begin = pos_;
            if (peek() == '-') ++pos_;
            if (peek() == '0') ++pos_;
            else { if (!digit(peek())) fail(); while (digit(peek())) ++pos_; }
            if (peek() == '.') {
                ++pos_; if (!digit(peek())) fail(); while (digit(peek())) ++pos_;
            }
            if (peek() == 'e' || peek() == 'E') {
                ++pos_; if (peek() == '+' || peek() == '-') ++pos_;
                if (!digit(peek())) fail();
                while (digit(peek())) ++pos_;
            }
            value.number = std::stod(source_.substr(begin, pos_ - begin));
            if (!std::isfinite(value.number)) fail();
            value.kind = JsonValue::Kind::NUMBER; return value;
        }
        for (const std::string literal : {"true", "false", "null"}) {
            if (source_.compare(pos_, literal.size(), literal) == 0) {
                pos_ += literal.size(); return value;
            }
        }
        fail();
    }
};
} // namespace eda
