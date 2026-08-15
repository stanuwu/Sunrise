#include "parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** 16 nested containers cap unknown-value checks and parser stack use. */
constexpr unsigned kMaximumJsonDepth = 16;

} // namespace

/** Checks and skips one JSON value of any type without allocating. */
bool Parser::skip_value(unsigned depth) noexcept {
    if (depth >= kMaximumJsonDepth) {
        return false;
    }
    whitespace();
    if (position_ >= input_.size()) {
        return false;
    }

    if (input_[position_] == '"') {
        std::string_view ignored;
        return string(ignored);
    }
    if (input_[position_] == '{') {
        // Check unknown members too, so bad JSON is never accepted.
        ++position_;
        if (consume('}')) {
            return true;
        }
        for (;;) {
            std::string_view ignored;
            if (!string(ignored) || !consume(':') || !skip_value(depth + 1)) {
                return false;
            }
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }
    if (input_[position_] == '[') {
        // Arrays use the same bounded recursion as unknown objects.
        ++position_;
        if (consume(']')) {
            return true;
        }
        for (;;) {
            if (!skip_value(depth + 1)) {
                return false;
            }
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }
    if (literal("true") || literal("false") || literal("null")) {
        return true;
    }
    return number();
}

/** Reads one expected structural character after whitespace. */
bool Parser::consume(char value) noexcept {
    whitespace();
    if (position_ >= input_.size() || input_[position_] != value) {
        return false;
    }
    ++position_;
    return true;
}

/** @return True when only trailing whitespace remains. */
bool Parser::at_end() noexcept {
    whitespace();
    return position_ == input_.size();
}

/** Skips JSON whitespace. Other control bytes are left alone. */
void Parser::whitespace() noexcept {
    while (position_ < input_.size()) {
        const char value = input_[position_];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            return;
        }
        ++position_;
    }
}

} // namespace sunrise::core::settings::parser
