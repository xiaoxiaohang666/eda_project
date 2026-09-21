#pragma once

#include <cmath>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace eda {

// Small lexer for the documented LEF/DEF subset. Records end at ';', not EOL.
class TextTokens {
public:
    explicit TextTokens(const std::string& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("cannot open " + path);
        std::string word;
        bool quoted = false;
        char ch;
        auto flush = [&] { if (!word.empty()) { words_.push_back(word); word.clear(); } };
        while (input.get(ch)) {
            if (ch == '\\') {
                word += ch;
                if (!input.get(ch)) throw std::runtime_error("truncated escape");
                word += ch;
            } else if (ch == '"') {
                word += ch;
                quoted = !quoted;
            } else if (!quoted && ch == '#') {
                flush();
                input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            } else if (!quoted && std::isspace(static_cast<unsigned char>(ch))) {
                flush();
            } else if (!quoted && (ch == ';' || ch == '(' || ch == ')')) {
                flush();
                words_.emplace_back(1, ch);
            } else {
                word += ch;
            }
        }
        if (input.bad() || quoted) throw std::runtime_error("truncated or unreadable input");
        flush();
    }

    bool done() const { return pos_ == words_.size(); }
    std::string peek() const { return done() ? "" : words_[pos_]; }
    std::string take() {
        if (done()) throw std::runtime_error("unexpected end of file");
        return words_[pos_++];
    }
    void expect(const std::string& expected) {
        const auto actual = take();
        if (actual != expected) throw std::runtime_error("expected " + expected + ", got " + actual);
    }
    double number() {
        const auto token = take();
        size_t used = 0;
        const double value = std::stod(token, &used);
        if (used != token.size() || !std::isfinite(value))
            throw std::runtime_error("invalid number: " + token);
        return value;
    }
    int integer() {
        const double value = number();
        if (value < 0 || value > std::numeric_limits<int>::max() || std::floor(value) != value)
            throw std::runtime_error("invalid nonnegative integer");
        return static_cast<int>(value);
    }
    void skip_statement() {
        while (take() != ";") {
            if (peek() == "END") throw std::runtime_error("missing semicolon before END");
        }
    }
    void skip_block(const std::string& name) {
        while (!done()) {
            if (take() == "END" && peek() == name) { take(); return; }
        }
        throw std::runtime_error("missing END " + name);
    }

private:
    std::vector<std::string> words_;
    size_t pos_{0};
};

} // namespace eda
