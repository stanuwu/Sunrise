#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include "../../../state/account/account_platform.h"
#include "../../logging/log.h"
#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** Space is the first printable ASCII value accepted by fixed string storage. */
constexpr unsigned int kMinimumPrintableAscii = 0x20;
/** Tilde is the last printable ASCII value accepted by fixed string storage. */
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
 * Decodes a nonempty printable-ASCII JSON string into fixed storage.
 * The storage reserves its last byte for the trailing null, so that byte bounds the decode.
 * @param encoded Borrowed encoded JSON string bytes.
 * @param output Receives the decoded bytes and a trailing null only on success.
 * @return True for 1 to Capacity minus 1 printable ASCII bytes.
 */
template <std::size_t Capacity>
[[nodiscard]] bool decode_bounded_string(std::string_view encoded,
                                         std::array<char, Capacity>& output) noexcept {
    std::array<char, Capacity> candidate{};
    std::size_t decodedCount = 0;
    for (std::size_t position = 0; position < encoded.size();) {
        char value = encoded[position++];
        if (value == '\\' && !decode_escape(encoded, position, value)) {
            return false;
        }
        const auto byte = static_cast<unsigned char>(value);
        if (byte < kMinimumPrintableAscii || byte > kMaximumPrintableAscii
            || decodedCount + 1 >= Capacity) {
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

/** Longest rejected token copied into the fallback line, so that line stays short. */
constexpr std::size_t kReportedTokenBytes = 32;

/** Destiny 2 only supports these languages; anything else falls back to English. */
constexpr std::array<std::string_view, 13> kSupportedLanguages{
    "english",
    "french",
    "german",
    "italian",
    "japanese",
    "brazilian",
    "spanish",
    "russian",
    "polish",
    "schinese",
    "tchinese",
    "latam",
    "koreana",
};

/** @param token Candidate Steam API language code. @return True when this build ships it. */
[[nodiscard]] bool is_supported_language(std::string_view token) noexcept {
    return std::find(kSupportedLanguages.begin(), kSupportedLanguages.end(), token)
           != kSupportedLanguages.end();
}

/**
 * Names a replaced language token. Settings are read before the log sinks exist, so this early
 * line is the only report.
 * @param encoded Borrowed encoded token that was not accepted.
 */
void report_language_fallback(std::string_view encoded) noexcept {
    std::array<char, 96> line{};
    // The token comes from the file, so its length is capped here rather than trusted.
    const std::string_view token = encoded.substr(0, std::min(encoded.size(), kReportedTokenBytes));
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings stage=language result=fallback token=%.*s",
                                      static_cast<int>(token.size()),
                                      token.data());
    if (written > 0) {
        log::early({line.data(), std::min(static_cast<std::size_t>(written), line.size() - 1)});
    }
}

/**
 * Resolves an authored Steam API language token, falling back to English for anything else.
 * One token is the only accepted shape, because GetCurrentGameLanguage answers with this value.
 * @param encoded Borrowed encoded JSON string bytes.
 * @return The matching supported code, or the English fallback.
 */
[[nodiscard]] std::array<char, steam::kLanguageCapacity>
resolve_language(std::string_view encoded) noexcept {
    std::array<char, steam::kLanguageCapacity> decoded{};
    if (decode_bounded_string(encoded, decoded)
        && is_supported_language(std::string_view(decoded.data()))) {
        return decoded;
    }
    report_language_fallback(encoded);
    return {"english"};
}

} // namespace

/** Parses Steam settings on top of the fixed defaults. */
bool Parser::compact_persona(steam::User& output) noexcept {
    std::string_view value;
    return string(value) && decode_bounded_string(value, output.personaName);
}

/** Parses Steam settings on top of the fixed defaults. */
bool Parser::steam_settings(steam::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    steam::Settings candidate = output;
    bool hasUser = false;
    bool hasLanguage = false;
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
        } else if (key == "language") {
            std::string_view value;
            if (hasLanguage || !string(value)) {
                return false;
            }
            candidate.language = resolve_language(value);
            hasLanguage = true;
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
    bool hasSteamId = false;
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
            if (hasPersonaName || !string(value)
                || !decode_bounded_string(value, candidate.personaName)) {
                return false;
            }
            hasPersonaName = true;
        } else if (key == "steam_id") {
            if (hasSteamId || !unsigned_value(candidate.steamId)
                || !state::account::platform::valid(candidate.steamId)) {
                return false;
            }
            hasSteamId = true;
            candidate.hasConfiguredSteamId = true;
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
