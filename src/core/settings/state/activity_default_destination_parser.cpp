#include <bitset>
#include <limits>

#include "../../../state/activity/defaults/activity_defaults_validation.h"
#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** An unsigned hash field is one fixed 32-bit value. */
constexpr std::uint64_t kMaximumHash = (std::numeric_limits<std::uint32_t>::max)();

/** Fields needed for one complete authored destination and numeric fallback row. */
enum class DestinationField : std::size_t {
    reason,
    previousActivityIndex,
    activityIndex,
    packageName,
    bubbleCount,
    statefulBubbleMask,
    initialSliceSet,
    spawnSetHash,
    count,
};

/**
 * Marks one destination field exactly once.
 * @param supplied Per-field tracker for repeats and completeness.
 * @param field Field named by the current JSON key.
 * @return False when the field was already present.
 */
[[nodiscard]] bool mark(std::bitset<static_cast<std::size_t>(DestinationField::count)>& supplied,
                        DestinationField field) noexcept {
    const std::size_t index = static_cast<std::size_t>(field);
    if (supplied.test(index)) {
        return false;
    }
    supplied.set(index);
    return true;
}

/**
 * Copies one already-checked package name into the fixed destination storage.
 * @param value Nonempty package bytes that fit the 40-byte schema field.
 * @param selection Cleared destination receiving the bytes and the length.
 */
void assign_package_name(std::string_view value,
                         state::activity::destination::DestinationSelection& selection) noexcept {
    selection.packageNameLength = static_cast<std::uint8_t>(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        selection.packageName[index] = static_cast<std::int8_t>(value[index]);
    }
}

} // namespace

/** Parses the optional activity settings object on top of the State defaults. */
bool Parser::activity_settings(state::activity::defaults::ActivityDefaults& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    bool hasDefaultDestination = false;
    bool hasArrivalOverrides = false;
    bool hasRosterKeyFromIdentity = false;
    bool hasRosterKeyOnAllSlots = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "default_destination") {
            if (hasDefaultDestination || !default_destination(output.defaultDestination)) {
                return false;
            }
            hasDefaultDestination = true;
        } else if (key == "arrival_overrides") {
            if (hasArrivalOverrides || !arrival_overrides(output)) {
                return false;
            }
            hasArrivalOverrides = true;
        } else if (key == "roster_key_from_identity") {
            if (hasRosterKeyFromIdentity || !boolean(output.rosterKeyFromIdentity)) {
                return false;
            }
            hasRosterKeyFromIdentity = true;
        } else if (key == "roster_key_on_all_slots") {
            if (hasRosterKeyOnAllSlots || !boolean(output.rosterKeyOnAllSlots)) {
                return false;
            }
            hasRosterKeyOnAllSlots = true;
        } else if (!skip_value(0)) {
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

/** Parses one local destination and its small numeric launch policy. */
bool Parser::default_destination(state::activity::defaults::DefaultDestination& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    std::bitset<static_cast<std::size_t>(DestinationField::count)> supplied;
    state::activity::defaults::DefaultDestination candidate{};
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "reason") {
            std::int64_t value = 0;
            if (!mark(supplied, DestinationField::reason) || !signed_integer(value)
                || value < state::activity::destination::kMinimumReason
                || value > state::activity::destination::kMaximumReason) {
                return false;
            }
            candidate.selection.reason = static_cast<std::int8_t>(value);
        } else if (key == "previous_activity_index") {
            std::int64_t value = 0;
            if (!mark(supplied, DestinationField::previousActivityIndex) || !signed_integer(value)
                || value < state::activity::destination::kAbsentActivityIndex
                || value > state::activity::destination::kMaximumActivityIndex) {
                return false;
            }
            candidate.selection.previousActivityIndex = static_cast<std::int16_t>(value);
        } else if (key == "activity_index") {
            std::int64_t value = 0;
            if (!mark(supplied, DestinationField::activityIndex) || !signed_integer(value)
                || value < 0 || value > state::activity::destination::kMaximumActivityIndex) {
                return false;
            }
            candidate.selection.activityIndex = static_cast<std::int16_t>(value);
        } else if (key == "package_name") {
            std::string_view value;
            if (!mark(supplied, DestinationField::packageName) || !string(value) || value.empty()
                || value.size() > state::activity::destination::kPackageNameCapacity) {
                return false;
            }
            assign_package_name(value, candidate.selection);
        } else if (key == "bubble_count") {
            std::uint64_t value = 0;
            if (!mark(supplied, DestinationField::bubbleCount) || !unsigned_integer(value)
                || value < state::activity::defaults::kMinimumBubbleCount
                || value > state::activity::defaults::kBubbleCapacity) {
                return false;
            }
            candidate.fallback.bubbleCount = static_cast<std::uint8_t>(value);
        } else if (key == "stateful_bubble_mask") {
            if (!mark(supplied, DestinationField::statefulBubbleMask)
                || !unsigned_value(candidate.fallback.statefulBubbleMask)) {
                return false;
            }
        } else if (key == "initial_slice_set") {
            std::uint64_t value = 0;
            if (!mark(supplied, DestinationField::initialSliceSet) || !unsigned_integer(value)
                || value > state::activity::defaults::kMaximumInitialSliceSet) {
                return false;
            }
            candidate.fallback.initialSliceSet = static_cast<std::uint16_t>(value);
        } else if (key == "spawn_set_hash") {
            std::uint64_t value = 0;
            if (!mark(supplied, DestinationField::spawnSetHash) || !unsigned_value(value)
                || value > kMaximumHash) {
                return false;
            }
            candidate.fallback.spawnSetHash = static_cast<std::uint32_t>(value);
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            // A complete row stops the package name and the numeric layout policy from mixing.
            if (!supplied.all() || !state::activity::defaults::valid(candidate)) {
                return false;
            }
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
