#include <cstddef>
#include <string_view>

#include "../../parser.h"

namespace sunrise::core::settings::parser {
namespace {

namespace external = client::external;

/** Space is the first printable ASCII value accepted by external-server text. */
constexpr unsigned int kMinimumPrintableAscii = 0x20;
/** Tilde is the last printable ASCII value accepted by external-server text. */
constexpr unsigned int kMaximumPrintableAscii = 0x7E;
/** Largest value one IPv4 octet can hold. */
constexpr unsigned int kMaximumOctet = 255;
/** A dotted quad has 3 separators. */
constexpr std::size_t kAddressSeparators = external::kAddressOctets - 1;
/** The Client compares all 37 token bytes, so a short GUID never matches. */
constexpr std::size_t kConfigGuidLength = external::kConfigGuidCapacity - 1;

/**
 * Copies printable ASCII into fixed storage.
 * JSON escapes are refused, not decoded: no supported value needs one.
 * @param encoded Borrowed JSON string.
 * @param output Receives the text and a trailing null only on success.
 * @return True for 1 to Capacity-1 printable ASCII bytes.
 */
template <std::size_t Capacity>
[[nodiscard]] bool decode_text(std::string_view encoded,
                               std::array<char, Capacity>& output) noexcept {
    if (encoded.empty() || encoded.size() >= Capacity) {
        return false;
    }
    std::array<char, Capacity> candidate{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto byte = static_cast<unsigned char>(encoded[index]);
        if (byte < kMinimumPrintableAscii || byte > kMaximumPrintableAscii || byte == '\\') {
            return false;
        }
        candidate[index] = encoded[index];
    }
    output = candidate;
    return true;
}

/**
 * Reads one decimal octet ending at a separator or at the end of the text.
 * @param text Complete dotted quad.
 * @param position Advanced past the octet and any separator after it.
 * @param output Receives the octet value.
 * @return True when 1 to 3 digits form a value of at most 255.
 */
[[nodiscard]] bool
read_octet(std::string_view text, std::size_t& position, unsigned char& output) noexcept {
    unsigned int value = 0;
    std::size_t digits = 0;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        value = value * 10 + static_cast<unsigned int>(text[position] - '0');
        ++position;
        ++digits;
        if (digits > 3 || value > kMaximumOctet) {
            return false;
        }
    }
    if (digits == 0) {
        return false;
    }
    output = static_cast<unsigned char>(value);
    return true;
}

/**
 * Splits a dotted quad into octets and copies it into wide storage.
 * A name is refused because the resolver forwards this text with AI_NUMERICHOST.
 * @param output Host settings whose text is already filled in.
 * @return True when the host text is a complete IPv4 dotted quad.
 */
[[nodiscard]] bool derive_address(external::Settings& output) noexcept {
    const std::string_view text(output.host.data());
    std::array<unsigned char, external::kAddressOctets> octets{};
    std::size_t position = 0;
    for (std::size_t index = 0; index < octets.size(); ++index) {
        if (!read_octet(text, position, octets[index])) {
            return false;
        }
        const bool separator = index < kAddressSeparators;
        if (separator && (position >= text.size() || text[position] != '.')) {
            return false;
        }
        position += separator ? 1 : 0;
    }
    if (position != text.size()) {
        return false;
    }
    output.address = octets;
    output.hostWide = {};
    for (std::size_t index = 0; index < text.size(); ++index) {
        output.hostWide[index] = static_cast<wchar_t>(text[index]);
    }
    return true;
}

} // namespace

/** Parses the external-server block on top of the fixed defaults. */
bool Parser::client_external_settings(external::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    external::Settings candidate = output;
    bool hasEnabled = false;
    bool hasHost = false;
    bool hasConfigUrl = false;
    bool hasConfigGuid = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "enabled") {
            if (hasEnabled || !boolean(candidate.enabled)) {
                return false;
            }
            hasEnabled = true;
        } else if (key == "host") {
            std::string_view value;
            if (hasHost || !string(value) || !decode_text(value, candidate.host)
                || !derive_address(candidate)) {
                return false;
            }
            hasHost = true;
        } else if (key == "config_url") {
            std::string_view value;
            if (hasConfigUrl || !string(value) || !decode_text(value, candidate.configUrl)) {
                return false;
            }
            hasConfigUrl = true;
        } else if (key == "config_guid") {
            std::string_view value;
            if (hasConfigGuid || !string(value) || value.size() != kConfigGuidLength
                || !decode_text(value, candidate.configGuid)) {
                return false;
            }
            hasConfigGuid = true;
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
