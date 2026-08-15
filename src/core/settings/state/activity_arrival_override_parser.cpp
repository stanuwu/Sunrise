#include <limits>

#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

namespace defaults = state::activity::defaults;

/** A bubble number picks one of the fixed 64 wire slots. */
constexpr std::uint64_t kMaximumBubble = 63;

/**
 * Copies one authored package name into its fixed row storage.
 * @param text Authored name.
 * @param output Row receiving the name and its length.
 * @return True when the name is nonempty and fits.
 */
[[nodiscard]] bool copy_name(std::string_view text, defaults::ArrivalOverride& output) noexcept {
    if (text.empty() || text.size() > output.name.size()) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        output.name[index] = text[index];
    }
    output.nameLength = static_cast<std::uint8_t>(text.size());
    return true;
}

} // namespace

/** Parses one authored arrival override row. */
bool Parser::arrival_override(defaults::ArrivalOverride& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    defaults::ArrivalOverride candidate{};
    bool hasName = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        std::uint64_t value = 0;
        if (key == "package_name") {
            std::string_view text;
            if (hasName || !string(text) || !copy_name(text, candidate)) {
                return false;
            }
            hasName = true;
        } else if (key == "bubble") {
            if (candidate.hasBubble || !unsigned_integer(value) || value > kMaximumBubble) {
                return false;
            }
            candidate.bubble = static_cast<std::uint8_t>(value);
            candidate.hasBubble = true;
        } else if (key == "spawn_set_hash") {
            if (candidate.hasSpawnSetHash || !unsigned_value(value)
                || value > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            candidate.spawnSetHash = static_cast<std::uint32_t>(value);
            candidate.hasSpawnSetHash = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            break;
        }
        if (!consume(',')) {
            return false;
        }
    }
    if (!hasName || (!candidate.hasBubble && !candidate.hasSpawnSetHash)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Parses the authored arrival override table. */
bool Parser::arrival_overrides(defaults::ActivityDefaults& output) noexcept {
    output.arrivalOverrides = {};
    output.arrivalOverrideCount = 0;
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        if (output.arrivalOverrideCount >= output.arrivalOverrides.size()
            || !arrival_override(output.arrivalOverrides[output.arrivalOverrideCount])) {
            return false;
        }
        ++output.arrivalOverrideCount;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
