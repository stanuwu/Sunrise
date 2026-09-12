#pragma once

#include <cstdint>

#include "../../unlocks/definition.h"

namespace sunrise::state::build_data::items {

/** Native pursuit bucket shared by quest items and bounties. */
inline constexpr std::uint8_t kPursuitBucketId = 40;
/** Missing saved quest rows read as zero; only this value permits a first-step write. */
inline constexpr std::int32_t kUnsetQuestValue = 0;
/** Policy: -1 cannot start a quest; existing -1 state must still be preserved. */
inline constexpr std::int32_t kInvalidQuestInitialValue = -1;

/** Authored initial value and bank row for the first member of a supported quest set. */
struct QuestInitialization {
    enum class Scope : std::uint8_t { none, account, character };
    std::int32_t value{};
    std::uint16_t row{};
    Scope scope{};

    bool operator==(const QuestInitialization&) const = default;
};

/**
 * An empty plan is valid; a nonempty plan must fit its saved bank.
 * @param quest First-step value, row, and scope from item metadata.
 * @return True for an empty plan or a supported value within its bank's capacity.
 */
[[nodiscard]] constexpr bool valid(const QuestInitialization& quest) noexcept {
    using Scope = QuestInitialization::Scope;
    if (quest.scope == Scope::none) {
        return quest.row == 0 && quest.value == kUnsetQuestValue;
    }
    return quest.value != kUnsetQuestValue && quest.value != kInvalidQuestInitialValue
           && ((quest.scope == Scope::account && quest.row < unlocks::kObjectiveValueCapacity)
               || (quest.scope == Scope::character
                   && quest.row < unlocks::kCharacterObjectValueCapacity));
}

/**
 * Preserve every nonzero value; step identifiers have no numeric progress order.
 * @param quest Validated first-step plan; may be empty.
 * @param before Current saved quest value.
 * @return The first-step value only when the plan is nonempty and saved state is unset.
 */
[[nodiscard]] constexpr std::int32_t initialized_value(const QuestInitialization& quest,
                                                       std::int32_t before) noexcept {
    return quest.scope != QuestInitialization::Scope::none && before == kUnsetQuestValue
               ? quest.value
               : before;
}

} // namespace sunrise::state::build_data::items
