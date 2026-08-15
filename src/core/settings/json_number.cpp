#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>

#include "parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** JSON integers are base 10 with no prefix. */
constexpr std::uint64_t kDecimalRadix = 10;
/** Authored 64-bit ids may be written as a quoted base-16 token. */
constexpr int kHexadecimalRadix = 16;
/** A hex id starts with the 2-character 0x marker. */
constexpr std::size_t kHexadecimalPrefixSize = 2;

} // namespace

/** Reads one unsigned decimal integer with no precision loss. */
bool Parser::unsigned_integer(std::uint64_t& output) noexcept {
    whitespace();
    const std::size_t start = position_;
    std::uint64_t value = 0;
    while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
        const std::uint64_t digit = static_cast<std::uint64_t>(input_[position_] - '0');
        if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / kDecimalRadix) {
            return false;
        }
        value = value * kDecimalRadix + digit;
        ++position_;
    }
    if (position_ == start || (position_ - start > 1 && input_[start] == '0')) {
        return false;
    }
    output = value;
    return true;
}

/** Reads an unsigned id as a JSON integer or a quoted hex token. */
bool Parser::unsigned_value(std::uint64_t& output) noexcept {
    whitespace();
    if (position_ >= input_.size() || input_[position_] != '"') {
        return unsigned_integer(output);
    }

    std::string_view token;
    if (!string(token)) {
        return false;
    }
    if (token.size() <= kHexadecimalPrefixSize || token[0] != '0'
        || (token[1] != 'x' && token[1] != 'X')) {
        return false;
    }
    token.remove_prefix(kHexadecimalPrefixSize);

    std::uint64_t parsed = 0;
    const auto result =
        std::from_chars(token.data(), token.data() + token.size(), parsed, kHexadecimalRadix);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    output = parsed;
    return true;
}

/** Reads one signed JSON integer. Fractions and exponents are refused. */
bool Parser::signed_integer(std::int64_t& output) noexcept {
    whitespace();
    const std::size_t start = position_;
    if (position_ < input_.size() && input_[position_] == '-') {
        ++position_;
    }
    if (position_ >= input_.size()) {
        return false;
    }
    if (input_[position_] == '0') {
        ++position_;
        if (position_ < input_.size()
            && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            return false;
        }
    } else {
        if (input_[position_] < '1' || input_[position_] > '9') {
            return false;
        }
        while (position_ < input_.size()
               && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    const char* begin = input_.data() + start;
    const char* end = input_.data() + position_;
    const auto result = std::from_chars(begin, end, output, static_cast<int>(kDecimalRadix));
    return result.ec == std::errc{} && result.ptr == end;
}

/** Reads one signed JSON integer into an 8-bit record value. */
bool Parser::signed_byte(std::int8_t& output) noexcept {
    std::int64_t value = 0;
    if (!signed_integer(value) || value < (std::numeric_limits<std::int8_t>::min)()
        || value > (std::numeric_limits<std::int8_t>::max)()) {
        return false;
    }
    output = static_cast<std::int8_t>(value);
    return true;
}

/** Reads one signed JSON integer into a 32-bit record value. */
bool Parser::signed_32(std::int32_t& output) noexcept {
    std::int64_t value = 0;
    if (!signed_integer(value) || value < (std::numeric_limits<std::int32_t>::min)()
        || value > (std::numeric_limits<std::int32_t>::max)()) {
        return false;
    }
    output = static_cast<std::int32_t>(value);
    return true;
}

/** Reads one finite JSON number into an authored float State value. */
bool Parser::floating_point(float& output) noexcept {
    whitespace();
    const std::size_t start = position_;
    if (!number()) {
        return false;
    }

    float parsed = 0.0F;
    const char* begin = input_.data() + start;
    const char* end = input_.data() + position_;
    const auto result = std::from_chars(begin, end, parsed, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(parsed)) {
        return false;
    }
    output = parsed;
    return true;
}

/** @return True when one complete JSON number is read. */
bool Parser::number() noexcept {
    whitespace();
    const std::size_t start = position_;
    if (position_ < input_.size() && input_[position_] == '-') {
        ++position_;
    }
    if (position_ >= input_.size()) {
        return false;
    }
    if (input_[position_] == '0') {
        ++position_;
    } else {
        if (input_[position_] < '1' || input_[position_] > '9') {
            return false;
        }
        while (position_ < input_.size()
               && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }
    if (position_ < input_.size() && input_[position_] == '.') {
        ++position_;
        const std::size_t fraction = position_;
        while (position_ < input_.size()
               && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (fraction == position_) {
            return false;
        }
    }
    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
        ++position_;
        if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
            ++position_;
        }
        const std::size_t exponent = position_;
        while (position_ < input_.size()
               && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (exponent == position_) {
            return false;
        }
    }
    return position_ != start;
}

} // namespace sunrise::core::settings::parser
