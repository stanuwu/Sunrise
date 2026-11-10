#include "season_pass_snapshot_runtime.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <new>
#include <span>

#include "../account/account_state.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../progression/season_pass_reward_catalog.h"
#include "../rewards/reward_resolver.h"
#include "../unlocks/definition.h"
#include "runtime.h"
#include "saved_selection_runtime.h"

namespace sunrise::state {
namespace {

namespace pass = progression::season_pass;

// An absent claim or acquisition flag lies past the account bank, so the bounds check skips it.
static_assert(unlocks::kAccountFlagCapacity <= build_data::season_pass::kUnavailableFlagIndex);
static_assert(unlocks::kAccountFlagCapacity <= build_data::rewards::kAbsent);

/** Saved values one pass evaluation reads, copied under one store lock. */
struct SavedPass {
    SavedSelection<AccountIdentity> view{};
    /** False while no character is selected. Only the banks' shared account rows are then read. */
    bool selected{};
    std::int32_t experience{};
    std::uint16_t rank{};
};

/** Copies XP, the account banks and the selected character's banks together. */
[[nodiscard]] bool read_saved_pass(SavedPass& saved, const char*& reason) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    reason = "account_state";
    switch (read_saved_selection(saved.view)) {
    case SavedSelectionRead::read:
        saved.selected = true;
        break;
    case SavedSelectionRead::unselected:
        // Every slot's read carries the shared account rows; slot 0's character rows go unread.
        if (!investment::store::read_unlocks(saved.view.banks, 0)) {
            return false;
        }
        break;
    case SavedSelectionRead::accountStorage:
    case SavedSelectionRead::accountInvalid:
    case SavedSelectionRead::bankStorage:
        return false;
    }
    saved.experience = seasonal_experience();
    saved.rank = seasonal_rank();
    return true;
}

/** @return True when the saved account flag is set. An absent index is never set. */
[[nodiscard]] bool flag_set(const unlocks::Table& banks, std::uint16_t index) noexcept {
    return index < banks.accountFlags.size() && banks.accountFlags[index] == unlocks::kFlagSet;
}

/** Evaluates one native row against the saved copies. A placeholder row stays unavailable. */
[[nodiscard]] SeasonPassRowStatus row_status(std::uint16_t rewardIndex,
                                             const SavedPass& saved) noexcept {
    SeasonPassRowStatus row{};
    build_data::season_pass::Reward reward{};
    if (!build_data::find_season_pass_reward(rewardIndex, reward)) {
        return row;
    }
    const unlocks::Table& banks = saved.view.banks;
    row.itemHash = reward.itemHash;
    row.requiredRank = reward.requiredRank;
    row.available = true;
    row.claimed = flag_set(banks, reward.claimFlagIndex);
    row.automatic = pass::automatic_row(reward);
    build_data::rewards::Item source{};
    row.acquired = build_data::find_reward_item(reward.itemIndex, source)
                   && source.definitionHash == reward.itemHash
                   && flag_set(banks, source.acquiredFlag);
    row.rankMet = reward.requiredRank <= saved.rank;
    // Conditions read a character's class and banks, so they wait for a selected character.
    if (saved.selected && reward.conditionCount <= reward.condition.size()) {
        bool met = false;
        row.conditionKnown = reward_resolution::eligible(
            std::span(reward.condition).first(reward.conditionCount), saved.view.context(), met);
        row.conditionMet = row.conditionKnown && met;
    }
    return row;
}

} // namespace

/** Reads one coherent saved pass view and evaluates every catalog row against it. */
bool read_season_pass_snapshot(SeasonPassSnapshot& output, const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    output = {};
    // Automatic rows and acquisition flags are read from the item and reward catalogs too.
    reason = build_data::reward_definitions_unsupported() ? "rewards_unsupported" : "not_ready";
    if (!season_perks_walkable()) {
        return false;
    }
    reason = "allocation";
    const std::unique_ptr<SavedPass> saved(new (std::nothrow) SavedPass{});
    if (!saved || !read_saved_pass(*saved, reason)) {
        return false;
    }
    const SavedSelection<AccountIdentity>& view = saved->view;
    output.accountSoid = view.account.primarySoid;
    output.characterSoid = saved->selected ? view.account.characters[view.slot].soid : 0;
    output.experience = saved->experience;
    output.rank = saved->rank;
    output.rowCount = (std::min)(build_data::season_pass_reward_count(), output.rows.size());
    for (std::size_t index = 0; index < output.rowCount; ++index) {
        output.rows[index] = row_status(static_cast<std::uint16_t>(index), *saved);
    }
    reason = nullptr;
    return true;
}

} // namespace sunrise::state
