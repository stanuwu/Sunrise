#include <algorithm>
#include <array>

#include "../../state/activity/defaults/activity_defaults_validation.h"
#include "parser.h"

namespace sunrise::core::settings::parser {

/**
 * Fills one flag bank from [start, length] runs. A run outside the bank is refused.
 * @param bank Expanded destination bank.
 * @return True when every run fits.
 */
bool Parser::flag_runs(std::span<std::uint8_t> bank) noexcept {
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        std::uint64_t start = 0;
        std::uint64_t length = 0;
        if (!consume('[') || !unsigned_integer(start) || !consume(',') || !unsigned_integer(length)
            || !consume(']') || length == 0 || start > bank.size()
            || length > bank.size() - start) {
            return false;
        }
        for (std::uint64_t offset = 0; offset < length; ++offset) {
            bank[static_cast<std::size_t>(start + offset)] = state::unlocks::kFlagSet;
        }
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/**
 * Fills one flag bank from plain indices.
 * @param bank Expanded destination bank.
 * @return True when every index fits.
 */
bool Parser::flag_indices(std::span<std::uint8_t> bank) noexcept {
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        std::uint64_t index = 0;
        if (!unsigned_integer(index) || index >= bank.size()) {
            return false;
        }
        bank[static_cast<std::size_t>(index)] = state::unlocks::kFlagSet;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/**
 * Fills one objective bank from [index, value] pairs.
 * @param bank Expanded destination bank.
 * @return True when every index fits.
 */
bool Parser::objective_values(std::span<std::int32_t> bank) noexcept {
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        std::uint64_t index = 0;
        std::int32_t value = 0;
        if (!consume('[') || !unsigned_integer(index) || !consume(',') || !signed_32(value)
            || !consume(']') || index >= bank.size()) {
            return false;
        }
        bank[static_cast<std::size_t>(index)] = value;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/**
 * Fills one progression bank from [definition_index, lane 0, lane 1, lane 2] rows.
 * Repeated rows for one definition merge by lane maximum, not by last write. The Client's
 * own definition-indexed bank merges the same way.
 * @param bank Authored lanes addressed by definition index.
 * @return True when every row names a possible definition and carries all 3 lanes.
 */
bool Parser::progression_values(state::unlocks::ProgressionBank& bank) noexcept {
    std::array<bool, state::build_data::progressions::kDefinitionCapacity> authored{};
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        std::uint64_t index = 0;
        state::unlocks::ProgressionLanes lanes{};
        if (!consume('[') || !unsigned_integer(index) || index >= bank.size()) {
            return false;
        }
        for (std::int32_t& lane : lanes) {
            if (!consume(',') || !signed_32(lane)) {
                return false;
            }
        }
        if (!consume(']')) {
            return false;
        }
        const auto slot = static_cast<std::size_t>(index);
        state::unlocks::ProgressionLanes& row = bank[slot];
        for (std::size_t lane = 0; lane < row.size(); ++lane) {
            // The first row of a definition is taken whole, so a negative lane is not clamped.
            row[lane] = authored[slot] ? (std::max)(row[lane], lanes[lane]) : lanes[lane];
        }
        authored[slot] = true;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/**
 * Parses the four authored unlock banks.
 * @param output Receives every expanded bank.
 * @return True when the object is valid JSON and every entry fits its bank.
 */
bool Parser::unlocks(state::unlocks::Table& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        bool parsed = false;
        if (key == "account_flag_runs") {
            parsed = flag_runs(output.accountFlags);
        } else if (key == "profile_flag_runs") {
            parsed = flag_runs(output.profileFlags);
        } else if (key == "character_flags") {
            parsed = flag_indices(output.characterFlags);
        } else if (key == "objective_values") {
            parsed = objective_values(output.objectiveValues);
        } else if (key == "character_flag_runs") {
            parsed = flag_runs(output.characterObjectFlags);
        } else if (key == "character_objective_values") {
            parsed = objective_values(output.characterObjectValues);
        } else if (key == "account_progressions") {
            parsed = progression_values(output.accountProgressions);
        } else if (key == "character_progressions") {
            parsed = progression_values(output.characterProgressions);
        } else {
            parsed = skip_value(0);
        }
        if (!parsed) {
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

/**
 * Parses authored State ids. Native mapping data is refused.
 * @param output Receives the initial account, unlock policy and activity defaults.
 * @return True when activity appears once and the cross-field rules hold.
 */
bool Parser::state_settings(Settings& output) noexcept {
    output.initialAccount = {};
    output.initialUnlocks = {};
    output.initialFamily5 = {};
    output.initialActivityDefaults = state::activity::defaults::authored();
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return true;
    }
    bool hasActivity = false;
    bool hasInvestment = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "account") {
            if (!account(output.initialAccount)) {
                return false;
            }
        } else if (key == "characters") {
            if (!characters(output.initialAccount)) {
                return false;
            }
        } else if (key == "unlocks") {
            if (!unlocks(output.initialUnlocks)) {
                return false;
            }
        } else if (key == "investment") {
            if (hasInvestment || !investment(output.initialFamily5)) {
                return false;
            }
            hasInvestment = true;
        } else if (key == "activity") {
            if (hasActivity || !activity_settings(output.initialActivityDefaults)) {
                return false;
            }
            hasActivity = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return state::account::valid_authored(output.initialAccount)
                   && state::activity::defaults::valid(output.initialActivityDefaults);
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/**
 * Parses an optional account id object.
 * @param output Receives the authored primary account SOID.
 * @return True for null or an object with one nonzero SOID and complete settings.
 */
bool Parser::account(state::AccountState& output) noexcept {
    if (literal("null")) {
        output = {};
        return true;
    }
    if (!consume('{')) {
        return false;
    }
    bool hasPrimarySoid = false;
    bool hasProfileSetupCompleted = false;
    bool hasSettings = false;
    bool hasDismantleRewards = false;
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "primary_soid") {
            if (hasPrimarySoid || !unsigned_value(output.primarySoid) || output.primarySoid == 0) {
                return false;
            }
            hasPrimarySoid = true;
        } else if (key == "profile_setup_completed") {
            if (hasProfileSetupCompleted || !boolean(output.profileSetupCompleted)) {
                return false;
            }
            hasProfileSetupCompleted = true;
        } else if (key == "settings") {
            if (hasSettings || !account_settings(output.settings)) {
                return false;
            }
            hasSettings = true;
        } else if (key == "profile_items") {
            if (!profile_items(output)) {
                return false;
            }
        } else if (key == "dismantle_rewards") {
            if (hasDismantleRewards || !dismantle_rewards(output)) {
                return false;
            }
            hasDismantleRewards = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return hasPrimarySoid && hasSettings;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
