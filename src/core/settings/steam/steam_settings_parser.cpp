#include <string_view>

#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** Space is the first printable ASCII value accepted by persona storage. */
constexpr unsigned int kMinimumPrintableAscii = 0x20;
/** Tilde is the last printable ASCII value accepted by persona storage. */
constexpr unsigned int kMaximumPrintableAscii = 0x7E;
/** Hex letter digits start at the value 10. */
constexpr unsigned int kHexadecimalAlphaOffset = 10;
/** A JSON Unicode escape carries 4 hex digits. */
constexpr std::size_t kUnicodeEscapeDigitCount = 4;

/**
 * Converts one hex digit to its number value.
 * @param output Receives a value from 0 to 15.
 * @return True for a JSON hex digit.
 */
[[nodiscard]] bool hex_value(char digit, unsigned int& output) noexcept {
    if (digit >= '0' && digit <= '9') {
        output = static_cast<unsigned int>(digit - '0');
        return true;
    }
    if (digit >= 'A' && digit <= 'F') {
        output = static_cast<unsigned int>(digit - 'A') + kHexadecimalAlphaOffset;
        return true;
    }
    if (digit >= 'a' && digit <= 'f') {
        output = static_cast<unsigned int>(digit - 'a') + kHexadecimalAlphaOffset;
        return true;
    }
    return false;
}

/**
 * Decodes one JSON escape into one ASCII byte.
 * @param encoded Borrowed encoded string bytes.
 * @param position Current input position after the escape prefix.
 * @param output Receives one decoded byte.
 * @return True when one whole supported escape was read.
 */
[[nodiscard]] bool
decode_escape(std::string_view encoded, std::size_t& position, char& output) noexcept {
    if (position >= encoded.size()) {
        return false;
    }
    const char escape = encoded[position++];
    switch (escape) {
    case '"':
    case '\\':
    case '/':
        output = escape;
        return true;
    case 'b':
        output = '\b';
        return true;
    case 'f':
        output = '\f';
        return true;
    case 'n':
        output = '\n';
        return true;
    case 'r':
        output = '\r';
        return true;
    case 't':
        output = '\t';
        return true;
    case 'u':
        break;
    default:
        return false;
    }

    if (encoded.size() - position < kUnicodeEscapeDigitCount) {
        return false;
    }
    unsigned int value = 0;
    for (std::size_t index = 0; index < kUnicodeEscapeDigitCount; ++index) {
        unsigned int digit = 0;
        if (!hex_value(encoded[position + index], digit)) {
            return false;
        }
        value = (value << 4U) | digit;
    }
    position += kUnicodeEscapeDigitCount;
    if (value < kMinimumPrintableAscii || value > kMaximumPrintableAscii) {
        return false;
    }
    output = static_cast<char>(value);
    return true;
}

/**
 * Decodes a nonempty persona into process-owned fixed storage.
 * @param encoded Borrowed encoded JSON string bytes.
 * @param output Receives the decoded bytes and a trailing null only on success.
 * @return True for 1 to 63 printable ASCII bytes.
 */
[[nodiscard]] bool decode_persona(std::string_view encoded,
                                  std::array<char, steam::kPersonaNameCapacity>& output) noexcept {
    std::array<char, steam::kPersonaNameCapacity> candidate{};
    std::size_t decodedCount = 0;
    for (std::size_t position = 0; position < encoded.size();) {
        char value = encoded[position++];
        if (value == '\\' && !decode_escape(encoded, position, value)) {
            return false;
        }
        const auto byte = static_cast<unsigned char>(value);
        if (byte < kMinimumPrintableAscii || byte > kMaximumPrintableAscii
            || decodedCount >= steam::kMaximumPersonaNameBytes) {
            return false;
        }
        candidate[decodedCount++] = value;
    }
    if (decodedCount == 0) {
        return false;
    }
    output = candidate;
    return true;
}

} // namespace

/** Parses Steam settings on top of the fixed defaults. */
bool Parser::steam_settings(steam::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    steam::Settings candidate = output;
    bool hasUser = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "user") {
            if (hasUser || !steam_user_settings(candidate.user)) {
                return false;
            }
            hasUser = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses the single local Steam user's fixed identity settings. */
bool Parser::steam_user_settings(steam::User& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    steam::User candidate = output;
    bool hasPersonaName = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "persona_name") {
            std::string_view value;
            if (hasPersonaName || !string(value) || !decode_persona(value, candidate.personaName)) {
                return false;
            }
            hasPersonaName = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
