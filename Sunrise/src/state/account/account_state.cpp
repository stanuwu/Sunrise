#include "account_state.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sunrise::state::account {
namespace {

/** Enough fixed storage for every account, character, profile-stack, and character-item key. */
inline constexpr std::size_t kIdentityCapacity =
    1 + inventory::kProfileItemCapacity
    + kCharacterCapacity * (1 + inventory::kEquipmentSlotCount + inventory::kCharacterItemCapacity);

/** @return True when a profile row carries no authored or runtime-owned value. */
[[nodiscard]] bool empty_profile_item(const inventory::ProfileItem& item) noexcept {
    return item.instanceSoid == 0 && item.definitionHash == 0 && item.quantity == 0
           && item.mutationSerial == 0;
}

/** Tier bits 1-5 are the native rarity ladder; bit 0 (no tier) is never a payout target. */
constexpr std::uint8_t kDismantleTierMaskBits = 0b0011'1110U;
constexpr std::uint8_t kDismantleClassMaskBits =
    static_cast<std::uint8_t>(DismantleGearClass::weapon)
    | static_cast<std::uint8_t>(DismantleGearClass::armor);

/** @return True when one unused dismantle policy row is canonical zero. */
[[nodiscard]] bool empty_dismantle_reward(const DismantleRewardPolicy& reward) noexcept {
    return reward.definitionHash == 0 && reward.quantity == 0 && reward.tierMask == 0
           && reward.classMask == 0 && reward.masterwork == DismantleMasterworkFilter::any;
}

/** Checks filled policy rows, uniqueness, and the zero tail. */
[[nodiscard]] bool valid_dismantle_rewards(const AccountState& state) noexcept {
    if (state.dismantleRewardCount > state.dismantleRewards.size()) {
        return false;
    }
    for (std::size_t index = 0; index < state.dismantleRewardCount; ++index) {
        const DismantleRewardPolicy& reward = state.dismantleRewards[index];
        if (reward.definitionHash == inventory::kNoDefinitionHash || reward.quantity <= 0
            || (reward.tierMask & ~kDismantleTierMaskBits) != 0
            || (reward.classMask & ~kDismantleClassMaskBits) != 0
            || reward.masterwork > DismantleMasterworkFilter::notMasterworked) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (same_dismantle_policy_key(state.dismantleRewards[prior], reward)) {
                return false;
            }
        }
    }
    const auto tail =
        state.dismantleRewards.cbegin() + static_cast<std::ptrdiff_t>(state.dismantleRewardCount);
    return std::all_of(tail, state.dismantleRewards.cend(), empty_dismantle_reward);
}

/** Adds one nonzero key to the bounded identity buffer. */
[[nodiscard]] bool append_identity(std::array<std::uint64_t, kIdentityCapacity>& identities,
                                   std::size_t& count,
                                   std::uint64_t soid) noexcept {
    if (soid == 0 || count >= identities.size()) {
        return false;
    }
    identities[count++] = soid;
    return true;
}

/** Checks the complete authored/runtime structure without consulting installed build data. */
[[nodiscard]] bool valid_impl(const AccountState& state, bool requireSettings = true) noexcept {
    if (state.profileItemCount > state.profileItems.size()
        || state.characterCount > state.characters.size()) {
        return false;
    }
    if (state.primarySoid == 0) {
        if (state.profileItemCount != 0 || state.characterCount != 0
            || state.dismantleRewardCount != 0 || state.settings.configured
            || state.settings.keyBindings.configured) {
            return false;
        }
        return std::all_of(
                   state.profileItems.cbegin(), state.profileItems.cend(), empty_profile_item)
               && std::all_of(state.dismantleRewards.cbegin(),
                              state.dismantleRewards.cend(),
                              empty_dismantle_reward);
    }
    if ((requireSettings && !settings::valid(state.settings)) || !valid_dismantle_rewards(state)) {
        return false;
    }

    std::array<std::uint64_t, kIdentityCapacity> identities{};
    std::size_t identityCount = 0;
    if (!append_identity(identities, identityCount, state.primarySoid)) {
        return false;
    }
    for (std::size_t index = 0; index < state.profileItemCount; ++index) {
        const inventory::ProfileItem& item = state.profileItems[index];
        if (item.definitionHash == inventory::kNoDefinitionHash || item.quantity <= 0
            || item.mutationSerial < 0
            || (item.instanceSoid != 0
                && !append_identity(identities, identityCount, item.instanceSoid))) {
            return false;
        }
    }
    const auto profileTail =
        state.profileItems.cbegin() + static_cast<std::ptrdiff_t>(state.profileItemCount);
    if (!std::all_of(profileTail, state.profileItems.cend(), empty_profile_item)) {
        return false;
    }

    bool selected = false;
    for (std::size_t index = 0; index < state.characterCount; ++index) {
        const CharacterState& character = state.characters[index];
        if (!append_identity(identities, identityCount, character.soid)
            || (character.selected && selected) || character.race > CharacterRace::exo
            || character.gender > CharacterGender::female
            || character.characterClass > CharacterClass::warlock
            || !std::isfinite(character.appearanceValue) || !inventory::valid(character.equipment)
            || !inventory::valid(character.inventory) || !inventory::valid(character.stacks)) {
            return false;
        }
        selected = selected || character.selected;
        for (const std::optional<inventory::Item>& item : character.equipment.slots) {
            if (item.has_value()
                && !append_identity(identities, identityCount, item->instanceSoid)) {
                return false;
            }
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.count; ++itemIndex) {
            if (!append_identity(identities,
                                 identityCount,
                                 character.inventory.values[itemIndex].instanceSoid)) {
                return false;
            }
        }
    }
    auto end = identities.begin() + static_cast<std::ptrdiff_t>(identityCount);
    std::sort(identities.begin(), end);
    return std::adjacent_find(identities.begin(), end) == end;
}

} // namespace

/**
 * Checks bounded ids and whole settings for every nonzero account.
 * @param state Account State to check.
 * @return True for empty State, or a whole account with unique ids.
 */
bool valid(const AccountState& state) noexcept {
    return valid_impl(state);
}

/** Checks settings-authored State before runtime-only profile stack identities are seeded. */
bool valid_authored(const AccountState& state) noexcept {
    return valid_impl(state);
}

bool valid_public(const AccountState& state) noexcept {
    return valid_impl(state, false);
}

/**
 * Finds the selected character without storing a second account-level key.
 * @param state Account snapshot read under the lock.
 * @return The selected character's nonzero SOID, or zero when none is selected.
 */
std::uint64_t selected_character_soid(const AccountState& state) noexcept {
    const std::size_t count = (std::min)(state.characterCount, state.characters.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (state.characters[index].selected) {
            return state.characters[index].soid;
        }
    }
    return 0;
}

/**
 * Finds the character the family-zero banner pair names.
 * The pair goes out before any pick, so it falls back to the first character.
 * @param state Account snapshot read under the lock.
 * @return The character's nonzero SOID, or zero when the account owns none.
 */
std::uint64_t banner_character_soid(const AccountState& state) noexcept {
    const std::uint64_t selected = selected_character_soid(state);
    if (selected != 0) {
        return selected;
    }
    return state.characterCount == 0 ? 0 : state.characters[0].soid;
}

} // namespace sunrise::state::account
