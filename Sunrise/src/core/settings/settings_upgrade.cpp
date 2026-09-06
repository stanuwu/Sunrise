/**
 * Boot-time repair of a settings file written against an older layout version. A member whose
 * value form changed cannot be read by the current parser, so the file would be refused and the
 * boot would end. Such a member is replaced with the bundled default before the parse. A member
 * that only changed name is renamed in place, so its authored value survives. A member this build
 * no longer reads is deleted, so the file stops carrying a key nothing acts on.
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

/** One replaced member, and the layout version that changed it. */
struct ReplacedMember {
    /** Quoted member name, so a value string cannot match it. */
    std::string_view name;
    /** Replaced only while the file is older than this version. */
    std::uint32_t version;
};

/**
 * Members replaced with the bundled default, each with the version that changed it.
 * A member is listed because its value form changed, or because its default changed.
 */
constexpr std::array<ReplacedMember, 9> kReplacedMembers{{
    {"\"key_bindings\"", 3},
    {"\"region_private\"", 5},
    {"\"topology\"", 5},
    {"\"characters\"", 5},
    {"\"profile_items\"", 7},
    // Version 8 turned the flat payout list into rows filtered by rarity, gear class and
    // masterwork state.
    {"\"dismantle_rewards\"", 8},
    // Version 13 turned these two on. A file that never carried them takes the new default; one
    // that carried the old value is corrected here.
    {"\"suppress_peer_relay\"", 13},
    {"\"activity_public_membership\"", 13},
    // Version 14 turned generation on. The whole block is taken, because "enabled" is not unique
    // in the document and the block also carries the lua_declarations default version 13 turned on.
    {"\"activity_sdk_generation\"", 14},
}};

/** One renamed member, and the layout version that renamed it. */
struct RenamedMember {
    /** Quoted old member name, so a value string cannot match it. */
    std::string_view from;
    /** Quoted new member name written in its place. */
    std::string_view to;
    /** Renamed only while the file is older than this version. */
    std::uint32_t version;
};

/**
 * Members renamed with their values kept, each with the version that renamed it.
 * Only the key text is replaced, so an authored value survives the rename.
 */
constexpr std::array<RenamedMember, 1> kRenamedMembers{{
    // Version 11 named the field for what it holds: the activity the client asked from.
    {"\"previous_activity_index\"", "\"source_activity_index\"", 11},
}};

/** One removed member, and the layout version that removed it. */
struct RemovedMember {
    /** Quoted member name, so a value string cannot match it. */
    std::string_view name;
    /** Removed only while the file is older than this version. */
    std::uint32_t version;
};

/** Members this build no longer reads, each with the version that removed it. */
constexpr std::array<RemovedMember, 1> kRemovedMembers{{
    {"\"force_join_request_ready\"", 12},
}};

/** One splice per replaced, renamed and removed member, plus the version member itself. */
constexpr std::size_t kSpliceCapacity =
    kReplacedMembers.size() + kRenamedMembers.size() + kRemovedMembers.size() + 1;
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
 * Finds one member's own name inside a document.
 * @param text Document text.
 * @param member Quoted member name.
 * @param start Receives the opening quote.
 * @param end Receives the byte after the closing quote.
 * @return True when the member is present as a key.
 */
[[nodiscard]] bool name_span(std::string_view text,
                             std::string_view member,
                             std::size_t& start,
                             std::size_t& end) noexcept {
    const std::size_t found = text.find(member);
    if (found == std::string_view::npos) {
        return false;
    }
    const std::size_t after = text.find_first_not_of(" \t\r\n", found + member.size());
    if (after == std::string_view::npos || text[after] != ':') {
        return false;
    }
    start = found;
    end = found + member.size();
    return true;
}

/**
 * Finds everything one member occupies, including the comma that joins it to a neighbour.
 * The comma after the value goes with the member. When the member is last, the comma before it
 * goes instead. Either way the object stays valid once the member is gone.
 * @param text Document text.
 * @param member Quoted member name.
 * @param start Receives the first byte to drop.
 * @param end Receives the byte after the last byte to drop.
 * @return True when the member is present as a key with a value.
 */
[[nodiscard]] bool member_span(std::string_view text,
                               std::string_view member,
                               std::size_t& start,
                               std::size_t& end) noexcept {
    std::size_t nameStart = 0;
    std::size_t nameEnd = 0;
    if (!name_span(text, member, nameStart, nameEnd) || !value_span(text, member, start, end)) {
        return false;
    }
    const std::string_view blanks = " \t\r\n";
    start = nameStart;
    while (start > 0 && blanks.find(text[start - 1]) != std::string_view::npos) {
        --start;
    }
    const std::size_t next = text.find_first_not_of(blanks, end);
    if (next != std::string_view::npos && text[next] == ',') {
        end = next + 1;
        return true;
    }
    // A last member takes the comma before it, or the object keeps a trailing one.
    if (start > 0 && text[start - 1] == ',') {
        --start;
    }
    return true;
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

    const std::uint32_t from = document_version(document);
    for (const ReplacedMember& member : kReplacedMembers) {
        // A file at or past that version keeps its own value, so a user choice is never lost.
        if (from >= member.version) {
            continue;
        }
        std::size_t replacementStart = 0;
        std::size_t replacementEnd = 0;
        if (!value_span(document, member.name, start, end)
            || !value_span(bundled, member.name, replacementStart, replacementEnd)) {
            // A member the file never carried needs no replacement.
            continue;
        }
        splices[count++] = {
            start, end, bundled.substr(replacementStart, replacementEnd - replacementStart)};
    }

    for (const RenamedMember& member : kRenamedMembers) {
        // A file at or past that version already carries the new name.
        if (from >= member.version || !name_span(document, member.from, start, end)) {
            continue;
        }
        splices[count++] = {start, end, member.to};
    }

    for (const RemovedMember& member : kRemovedMembers) {
        // A file at or past that version was written without the member.
        if (from >= member.version || !member_span(document, member.name, start, end)) {
            continue;
        }
        splices[count++] = {start, end, {}};
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
