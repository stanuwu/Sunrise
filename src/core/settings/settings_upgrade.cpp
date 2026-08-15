/**
 * Boot-time repair of a settings file written against an older layout version. A member whose
 * value form changed cannot be read by the current parser, so the file would be refused and the
 * boot would end. Such a member is replaced with the bundled default before the parse.
 */

#include "settings_upgrade.h"

#include <array>
#include <cstdint>
#include <cstdio>

#include "settings.h"

namespace sunrise::core::settings::upgrade {
namespace {

/** The layout version member, quoted so a value string cannot match it. */
constexpr std::string_view kVersionMember = "\"version\"";
/** Members replaced with the bundled default because their value form changed. */
constexpr std::array<std::string_view, 1> kReplacedMembers{"\"key_bindings\""};
/** One splice per replaced member, plus the version member itself. */
constexpr std::size_t kSpliceCapacity = kReplacedMembers.size() + 1;
/** Room for the version member and its digits when the file predates versioning. */
constexpr std::size_t kVersionTextCapacity = 32;

/** One region of the document and the text that takes its place. */
struct Splice {
    std::size_t start;
    std::size_t end;
    std::string_view text;
};

/**
 * Steps over one JSON string.
 * @param text Document text.
 * @param position Index of the opening quote.
 * @return Index after the closing quote, or the length when it never closes.
 */
[[nodiscard]] std::size_t skip_string(std::string_view text, std::size_t position) noexcept {
    for (++position; position < text.size(); ++position) {
        if (text[position] == '\\') {
            ++position;
            continue;
        }
        if (text[position] == '"') {
            return position + 1;
        }
    }
    return text.size();
}

/**
 * Steps over one JSON value of any type. Strings are skipped whole, so a brace inside one cannot
 * close an object.
 * @param text Document text.
 * @param position Index of the first byte of the value.
 * @return Index after the value.
 */
[[nodiscard]] std::size_t skip_value(std::string_view text, std::size_t position) noexcept {
    if (position >= text.size()) {
        return text.size();
    }
    if (text[position] == '"') {
        return skip_string(text, position);
    }
    if (text[position] != '{' && text[position] != '[') {
        std::size_t end = position;
        while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != ']') {
            ++end;
        }
        // A scalar runs to its separator, so trailing layout belongs to the document, not the
        // value.
        while (end > position
               && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'
                   || text[end - 1] == '\n')) {
            --end;
        }
        return end;
    }
    std::size_t depth = 0;
    for (; position < text.size(); ++position) {
        const char value = text[position];
        if (value == '"') {
            position = skip_string(text, position) - 1;
            continue;
        }
        if (value == '{' || value == '[') {
            ++depth;
            continue;
        }
        if (value == '}' || value == ']') {
            --depth;
            if (depth == 0) {
                return position + 1;
            }
        }
    }
    return text.size();
}

/**
 * Finds one member's value inside a document.
 * @param text Document text.
 * @param member Quoted member name.
 * @param start Receives the first byte of the value.
 * @param end Receives the byte after the value.
 * @return True when the member is present with a value.
 */
[[nodiscard]] bool value_span(std::string_view text,
                              std::string_view member,
                              std::size_t& start,
                              std::size_t& end) noexcept {
    const std::size_t found = text.find(member);
    if (found == std::string_view::npos) {
        return false;
    }
    std::size_t position = found + member.size();
    const std::string_view blanks = " \t\r\n";
    position = text.find_first_not_of(blanks, position);
    if (position == std::string_view::npos || text[position] != ':') {
        return false;
    }
    position = text.find_first_not_of(blanks, position + 1);
    if (position == std::string_view::npos) {
        return false;
    }
    start = position;
    end = skip_value(text, position);
    return end > start;
}

/**
 * Reads the layout version a document was written against.
 * @param document Document text.
 * @return The version, or zero when the member is absent or unreadable.
 */
[[nodiscard]] std::uint32_t document_version(std::string_view document) noexcept {
    std::size_t start = 0;
    std::size_t end = 0;
    if (!value_span(document, kVersionMember, start, end)) {
        return 0;
    }
    std::uint32_t version = 0;
    for (std::size_t position = start; position < end; ++position) {
        const char digit = document[position];
        if (digit < '0' || digit > '9') {
            return 0;
        }
        version = version * 10 + static_cast<std::uint32_t>(digit - '0');
    }
    return version;
}

} // namespace

/** Reports whether the document predates the layout this build reads. */
bool needed(std::string_view document) noexcept {
    return document_version(document) < kSettingsVersion;
}

/** Replaces the changed members with the bundled defaults and stamps the current version. */
bool apply(std::string_view document,
           std::string_view bundled,
           std::span<char> output,
           std::size_t& written) noexcept {
    written = 0;
    std::array<Splice, kSpliceCapacity> splices{};
    std::size_t count = 0;

    std::array<char, kVersionTextCapacity> versionText{};
    std::size_t start = 0;
    std::size_t end = 0;
    if (value_span(document, kVersionMember, start, end)) {
        const int length = std::snprintf(
            versionText.data(), versionText.size(), "%u", static_cast<unsigned>(kSettingsVersion));
        if (length <= 0) {
            return false;
        }
        splices[count++] = {start, end, {versionText.data(), static_cast<std::size_t>(length)}};
    } else {
        // A file that predates versioning carries no member to replace, so one is added first.
        const std::size_t root = document.find('{');
        if (root == std::string_view::npos) {
            return false;
        }
        const int length = std::snprintf(versionText.data(),
                                         versionText.size(),
                                         "\"version\": %u,",
                                         static_cast<unsigned>(kSettingsVersion));
        if (length <= 0) {
            return false;
        }
        splices[count++] = {
            root + 1, root + 1, {versionText.data(), static_cast<std::size_t>(length)}};
    }

    for (const std::string_view member : kReplacedMembers) {
        std::size_t replacementStart = 0;
        std::size_t replacementEnd = 0;
        if (!value_span(document, member, start, end)
            || !value_span(bundled, member, replacementStart, replacementEnd)) {
            // A member the file never carried needs no replacement.
            continue;
        }
        splices[count++] = {
            start, end, bundled.substr(replacementStart, replacementEnd - replacementStart)};
    }

    for (std::size_t index = 1; index < count; ++index) {
        const Splice held = splices[index];
        std::size_t position = index;
        while (position > 0 && splices[position - 1].start > held.start) {
            splices[position] = splices[position - 1];
            --position;
        }
        splices[position] = held;
    }

    std::size_t read = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const Splice& splice = splices[index];
        const std::size_t copied = splice.start - read;
        if (written + copied + splice.text.size() > output.size()) {
            return false;
        }
        document.copy(output.data() + written, copied, read);
        written += copied;
        splice.text.copy(output.data() + written, splice.text.size());
        written += splice.text.size();
        read = splice.end;
    }
    const std::size_t tail = document.size() - read;
    if (written + tail > output.size()) {
        return false;
    }
    document.copy(output.data() + written, tail, read);
    written += tail;
    return true;
}

} // namespace sunrise::core::settings::upgrade
