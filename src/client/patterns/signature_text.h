#pragma once

// Compiles IDA-style signature text such as "48 8B ? C3" into fixed pattern bytes.
// Everything here runs at compile time; malformed text fails the build.

#include <array>
#include <cstddef>
#include <string_view>

#include "registry.h"

namespace sunrise::client::patterns {
namespace signature_detail {

/** One unknown byte is written as a single question mark. */
inline constexpr char kWildcard = '?';
/** Tokens are separated by one or more spaces. */
inline constexpr char kSeparator = ' ';
/** An exact byte token is 2 hex digits. */
inline constexpr std::size_t kByteTokenLength = 2;
/** Hex digits above 9 start at this value. */
inline constexpr unsigned kFirstLetterValue = 10;
/** Returned for any character that is not a hex digit. */
inline constexpr unsigned kNotHexDigit = 16;
/** The high digit of a byte token is shifted by one nibble. */
inline constexpr unsigned kHighDigitShift = 4;

/** Declared and never defined, so calling it rejects malformed text at compile time. */
void malformed_signature_text();

/** @param digit ASCII character. @return Its value, or kNotHexDigit when it is not hex. */
[[nodiscard]] constexpr unsigned hex_value(char digit) noexcept {
    if (digit >= '0' && digit <= '9') {
        return static_cast<unsigned>(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f') {
        return static_cast<unsigned>(digit - 'a') + kFirstLetterValue;
    }
    if (digit >= 'A' && digit <= 'F') {
        return static_cast<unsigned>(digit - 'A') + kFirstLetterValue;
    }
    return kNotHexDigit;
}

/** @param token One separator-free token. @return True for a wildcard or two hex digits. */
[[nodiscard]] constexpr bool valid_token(std::string_view token) noexcept {
    if (token.size() == 1) {
        return token[0] == kWildcard;
    }
    return token.size() == kByteTokenLength && hex_value(token[0]) != kNotHexDigit
           && hex_value(token[1]) != kNotHexDigit;
}

/** @param text Signature text. @param start Token start. @return Index one past the token. */
[[nodiscard]] constexpr std::size_t token_end(std::string_view text, std::size_t start) noexcept {
    std::size_t end = start;
    while (end < text.size() && text[end] != kSeparator) {
        ++end;
    }
    return end;
}

/** @param token Validated exact token. @return The byte it encodes. */
[[nodiscard]] constexpr std::byte token_byte(std::string_view token) noexcept {
    const unsigned value = (hex_value(token[0]) << kHighDigitShift) | hex_value(token[1]);
    return static_cast<std::byte>(value);
}

} // namespace signature_detail

/**
 * Counts the byte tokens in one signature text.
 * @param text Space-separated 2-digit hex bytes and question-mark wildcards.
 * @return Token count.
 */
[[nodiscard]] consteval std::size_t signature_length(std::string_view text) {
    std::size_t count = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        if (text[index] == signature_detail::kSeparator) {
            ++index;
            continue;
        }
        const std::size_t end = signature_detail::token_end(text, index);
        if (!signature_detail::valid_token(text.substr(index, end - index))) {
            signature_detail::malformed_signature_text();
        }
        ++count;
        index = end;
    }
    return count;
}

/**
 * Compiles one signature text into fixed pattern bytes.
 * @tparam Length Token count, taken from signature_length on the same text.
 * @param text Space-separated 2-digit hex bytes and question-mark wildcards.
 * @return Pattern bytes in text order; a wildcard token leaves its entry inexact.
 */
template <std::size_t Length>
[[nodiscard]] consteval std::array<PatternByte, Length> signature(std::string_view text) {
    std::array<PatternByte, Length> bytes{};
    std::size_t count = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        if (text[index] == signature_detail::kSeparator) {
            ++index;
            continue;
        }
        const std::size_t end = signature_detail::token_end(text, index);
        const std::string_view token = text.substr(index, end - index);
        if (count == Length || !signature_detail::valid_token(token)) {
            signature_detail::malformed_signature_text();
        }
        if (token[0] != signature_detail::kWildcard) {
            bytes[count] = PatternByte{signature_detail::token_byte(token), true};
        }
        ++count;
        index = end;
    }
    if (count != Length) {
        signature_detail::malformed_signature_text();
    }
    return bytes;
}

} // namespace sunrise::client::patterns
