#include <algorithm>

#include "validation.h"

namespace sunrise::state::entitlements {
namespace {

/** Lowest printable ASCII byte accepted in an authored name. */
constexpr char kFirstPrintable = 0x20;
/** Highest printable ASCII byte accepted in an authored name. */
constexpr char kLastPrintable = 0x7E;
/** Application ids are declared to SignOn as unsigned 32-bit values. */
constexpr std::uint64_t kMaximumApplicationId = 0xFFFFFFFF;

/**
 * Parses a decimal application id from an authored name.
 * @param text Candidate name.
 * @param value Receives the parsed id.
 * @return True only for a nonempty decimal value in range, with no other characters.
 */
[[nodiscard]] bool application_id(std::string_view text, std::uint32_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        parsed = parsed * 10 + static_cast<std::uint64_t>(digit - '0');
        if (parsed > kMaximumApplicationId) {
            return false;
        }
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

} // namespace

/** Checks one authored entitlement definition. */
bool valid(const Entitlement& candidate) noexcept {
    if (candidate.nameLength == 0 || candidate.nameLength >= kNameCapacity) {
        return false;
    }
    const std::string_view text = name_of(candidate);
    const bool printable = std::all_of(text.begin(), text.end(), [](char byte) noexcept {
        return byte >= kFirstPrintable && byte <= kLastPrintable;
    });
    if (!printable) {
        return false;
    }
    // An application-owned definition declares the numeric name itself, so it must parse.
    std::uint32_t identifier = 0;
    return candidate.ownership != Ownership::application || application_id(text, identifier);
}

/** Checks one complete authored ownership table. */
bool valid(const Table& candidate) noexcept {
    if (candidate.count > kCapacity) {
        return false;
    }
    const std::span<const Entitlement> entries = used(candidate);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (!valid(entries[index])) {
            return false;
        }
        // Duplicate names would map two handles onto one definition the Client finds by name.
        for (std::size_t other = 0; other < index; ++other) {
            if (name_of(entries[index]) == name_of(entries[other])) {
                return false;
            }
        }
    }
    return true;
}

/** Finds the id one owned definition is declared by. */
bool owned_identifier(const Table& table, std::size_t index, std::uint32_t& identifier) noexcept {
    if (index >= table.count) {
        return false;
    }
    const Entitlement& entry = table.entries[index];
    if (entry.ownership == Ownership::handle) {
        identifier = handle(index);
        return true;
    }
    return entry.ownership == Ownership::application && application_id(name_of(entry), identifier);
}

} // namespace sunrise::state::entitlements
