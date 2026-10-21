/** Dismantle staging: the payout it credits and the after-image it is committed against. */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "../../core/logging/log.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state.h"
#include "state_account_transaction_helpers.h"
#include "state_rolled_socket_plugs.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace runtime::detail {

namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;
namespace family4_loadout = middleware::datagen::family4::loadout;

/** Writes one exhaustive item-dismantle transaction checkpoint. */
void report_dismantle(std::string_view stage,
                      std::string_view result,
                      std::string_view reason,
                      std::uint32_t definitionHash,
                      std::uint64_t characterSoid,
                      std::uint64_t instanceSoid,
                      std::size_t inventoryIndex,
                      std::uint16_t inventoryRow,
                      std::uint8_t equipmentSlot,
                      std::size_t movedItemCount,
                      std::uint32_t nextInventorySerial) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=dismantle stage=%.*s result=%.*s reason=%.*s definition_hash=0x%08X "
                      "character=0x%llX instance=0x%llX inventory_index=%zu inventory_row=%u "
                      "equipment_slot=%u moved_items=%zu next_serial=%u",
                      static_cast<int>(stage.size()),
                      stage.data(),
                      static_cast<int>(result.size()),
                      result.data(),
                      static_cast<int>(reason.size()),
                      reason.data(),
                      definitionHash,
                      static_cast<unsigned long long>(characterSoid),
                      static_cast<unsigned long long>(instanceSoid),
                      inventoryIndex,
                      static_cast<unsigned>(inventoryRow),
                      static_cast<unsigned>(equipmentSlot),
                      movedItemCount,
                      nextInventorySerial);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Records how the dismantled item was classified and how many payout materials matched. */
void report_dismantle_reward_match(std::uint32_t definitionHash,
                                   std::uint8_t tier,
                                   std::uint8_t gearClass,
                                   bool isMasterworked,
                                   std::size_t materialCount) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=dismantle stage=reward result=ok reason=matched "
                                    "definition_hash=0x%08X tier=%u gear_class=%u masterworked=%u "
                                    "materials=%zu",
                                    definitionHash,
                                    static_cast<unsigned>(tier),
                                    static_cast<unsigned>(gearClass),
                                    isMasterworked ? 1U : 0U,
                                    materialCount);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Records one payout row that could not be credited, so a silent zero payout is visible. */
void report_dismantle_reward_dropped(std::string_view reason,
                                     std::uint32_t definitionHash,
                                     std::int32_t policyQuantity,
                                     std::int32_t previousQuantity,
                                     std::int32_t maxStackSize) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=dismantle stage=reward result=dropped reason=%.*s "
                                    "definition_hash=0x%08X policy_quantity=%d held=%d "
                                    "max_stack=%d",
                                    static_cast<int>(reason.size()),
                                    reason.data(),
                                    definitionHash,
                                    policyQuantity,
                                    previousQuantity,
                                    maxStackSize);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/**
 * @return The gear class one native equipment slot belongs to, or 0 outside the gear the
 *         supported client pays materials for. Native slot numbers are not the semantic enum
 *         order (kinetic is 7, energy 8, heavy 9), so the check goes through the semantic map.
 */
[[nodiscard]] std::uint8_t gear_class_of(std::uint8_t nativeSlot) noexcept {
    using EquipmentSlot = authored_inventory::EquipmentSlot;
    std::size_t semanticIndex = authored_inventory::kEquipmentSlotCount;
    if (!semantic_equipment_slot(nativeSlot, semanticIndex)) {
        return 0;
    }
    switch (static_cast<EquipmentSlot>(semanticIndex)) {
    case EquipmentSlot::kinetic:
    case EquipmentSlot::energy:
    case EquipmentSlot::heavy:
        return static_cast<std::uint8_t>(DismantleGearClass::weapon);
    case EquipmentSlot::helmet:
    case EquipmentSlot::gauntlets:
    case EquipmentSlot::chest:
    case EquipmentSlot::legs:
    case EquipmentSlot::classItem:
        return static_cast<std::uint8_t>(DismantleGearClass::armor);
    default:
        return 0;
    }
}

/** Per-stat-row tally of one lane's pool, enough to recognise a masterwork tier ladder. */
struct LadderTally {
    /** A stat row is one byte, so this covers every row a definition can name. */
    static constexpr std::size_t kRowCount = 256;
    /** Distinct values one row can track, which is the width of the seen mask. */
    static constexpr std::size_t kValueBits = 64;
    std::array<std::uint16_t, kRowCount> members{};
    std::array<std::int32_t, kRowCount> greatest{};
    /** Bit per distinct value below kValueBits; a larger value marks the row unusable. */
    std::array<std::uint64_t, kRowCount> seen{};
    std::array<bool, kRowCount> overflow{};
};

/** Folds one pool member's stats into the tally. */
bool tally_member(void* context, std::uint16_t plugIndex) noexcept {
    auto& tally = *static_cast<LadderTally*>(context);
    item_details::Definition detail{};
    if (!build_data::find_configured_item_detail(plugIndex, detail)
        || detail.statCount > detail.stats.size()) {
        return true;
    }
    for (std::size_t index = 0; index < detail.statCount; ++index) {
        const item_details::Stat& stat = detail.stats[index];
        ++tally.members[stat.row];
        tally.greatest[stat.row] = (std::max)(tally.greatest[stat.row], stat.value);
        if (stat.value < 0 || stat.value >= static_cast<std::int32_t>(LadderTally::kValueBits)) {
            tally.overflow[stat.row] = true;
        } else {
            tally.seen[stat.row] |= 1ULL << static_cast<unsigned>(stat.value);
        }
    }
    return true;
}

/**
 * @return True when the plug sits high enough on its lane's masterwork ladder, a pool whose
 *         plugs share one stat row with a distinct value each. Assumed: the weapon-at-top and
 *         armor-at-halfway cut-offs come from service behaviour, not from the Client.
 */
[[nodiscard]] bool on_masterwork_ladder(const item_details::Definition& target,
                                        std::uint8_t lane,
                                        const item_details::Definition& plug,
                                        bool weapon) noexcept {
    // Below three rungs a pool cannot be told apart from a mod pool that happens not to repeat.
    constexpr std::uint16_t kMinimumLadderRungs = 3;
    if (plug.statCount == 0 || plug.statCount > plug.stats.size()) {
        return false;
    }
    LadderTally tally{};
    if (!build_data::visit_socket_plug_pool(target.definitionIndex, lane, &tally_member, &tally)) {
        return false;
    }
    // The ladder row is the plug's stat row most of the pool shares.
    std::size_t ladderRow = LadderTally::kRowCount;
    for (std::size_t index = 0; index < plug.statCount; ++index) {
        const std::uint8_t row = plug.stats[index].row;
        if (ladderRow == LadderTally::kRowCount || tally.members[row] > tally.members[ladderRow]) {
            ladderRow = row;
        }
    }
    if (ladderRow >= LadderTally::kRowCount || tally.overflow[ladderRow]
        || tally.members[ladderRow] < kMinimumLadderRungs
        || std::popcount(tally.seen[ladderRow]) != tally.members[ladderRow]) {
        return false;
    }
    std::int32_t value = 0;
    for (std::size_t index = 0; index < plug.statCount; ++index) {
        if (plug.stats[index].row == ladderRow) {
            value = plug.stats[index].value;
        }
    }
    const std::int32_t top = tally.greatest[ladderRow];
    return weapon ? value == top : value * 2 >= top;
}

/**
 * @return True when the item is masterworked for the payout: a lane holds a rolled result plug
 *         (a Year-1 masterwork), or a plug high enough on its lane's tier ladder. An item still
 *         on its native defaults is read through the definition's initial plugs, since
 *         default-equipped gear can ship masterworked.
 */
[[nodiscard]] bool masterworked(const authored_inventory::Item& item,
                                const item_details::Definition& detail,
                                bool weapon) noexcept {
    const bool authored = item.sockets.policy == authored_inventory::SocketPolicy::authored;
    for (std::size_t lane = 0;
         lane < detail.ordinarySocketCount && lane < authored_inventory::kPlugCapacity;
         ++lane) {
        build_data::items::Definition plug{};
        if (authored) {
            const std::optional<std::uint32_t>& hash = item.sockets.plugs[lane];
            if (!hash.has_value() || !build_data::find_item_definition_hash(*hash, plug)
                || plug.definitionHash != *hash) {
                continue;
            }
        } else {
            const std::uint16_t plugIndex = detail.initialPlugIndices[lane];
            if (plugIndex == item_details::kUnavailableItemIndex
                || !build_data::find_item_definition_index(plugIndex, plug)
                || plug.definitionIndex != plugIndex) {
                continue;
            }
        }
        if (is_rolled_result(plug.definitionHash)) {
            return true;
        }
        item_details::Definition plugDetail{};
        if (build_data::find_configured_item_detail(plug.definitionIndex, plugDetail)
            && plugDetail.definitionHash == plug.definitionHash
            && on_masterwork_ladder(detail, static_cast<std::uint8_t>(lane), plugDetail, weapon)) {
            return true;
        }
    }
    return false;
}

/** @return True when one policy row pays for the dismantled item. */
[[nodiscard]] bool policy_matches(const DismantleRewardPolicy& policy,
                                  std::uint8_t tier,
                                  std::uint8_t gearClass,
                                  bool isMasterworked) noexcept {
    // The mask is eight bits wide, so a manifest tier past it cannot be selected. Tested before
    // the shift, which is undefined once the tier reaches the width of the shifted type.
    constexpr std::uint8_t kTierBits = 8;
    if (policy.tierMask != 0 && (tier >= kTierBits || (policy.tierMask & (1U << tier)) == 0)) {
        return false;
    }
    if (policy.classMask != 0 && (policy.classMask & gearClass) == 0) {
        return false;
    }
    switch (policy.masterwork) {
    case DismantleMasterworkFilter::masterworked:
        return isMasterworked;
    case DismantleMasterworkFilter::notMasterworked:
        return !isMasterworked;
    default:
        return true;
    }
}

/**
 * Credits the supported client's ordinary weapon/armor dismantle payout.
 * Matching policy rows are summed per material first, so one material lands as one credited row.
 * A capped stack loses only the overflow and says so in the log. Each credited row gets a serial.
 */
[[nodiscard]] bool
apply_dismantle_rewards(const AccountState& before,
                        const authored_inventory::Item& dismantledItem,
                        const build_data::items::Definition& dismantledDefinition,
                        const item_details::Definition& dismantledDetail,
                        std::uint8_t equipmentSlot,
                        AccountState& after,
                        std::array<DismantleReward, kDismantleRewardCapacity>& rewards,
                        std::size_t& rewardCount) noexcept {
    after = before;
    rewards = {};
    rewardCount = 0;
    if (!valid_profile_inventory(before)) {
        return false;
    }
    const std::uint8_t gearClass = gear_class_of(equipmentSlot);
    if (gearClass == 0) {
        return true;
    }
    const std::uint8_t tier = dismantledDefinition.tier;
    const bool isMasterworked =
        masterworked(dismantledItem,
                     dismantledDetail,
                     gearClass == static_cast<std::uint8_t>(DismantleGearClass::weapon));

    // Sum the matching rows per material before crediting anything.
    std::array<DismantleRewardPolicy, kDismantleRewardPolicyCapacity> payout{};
    std::size_t payoutCount = 0;
    for (std::size_t policyIndex = 0; policyIndex < before.dismantleRewardCount; ++policyIndex) {
        const DismantleRewardPolicy& policy = before.dismantleRewards[policyIndex];
        if (!policy_matches(policy, tier, gearClass, isMasterworked)) {
            continue;
        }
        std::size_t slot = payoutCount;
        for (std::size_t index = 0; index < payoutCount; ++index) {
            if (payout[index].definitionHash == policy.definitionHash) {
                slot = index;
                break;
            }
        }
        if (slot == payoutCount) {
            if (payoutCount >= payout.size()) {
                return false;
            }
            payout[payoutCount++] = {policy.definitionHash, 0};
        }
        if (policy.quantity > (std::numeric_limits<std::int32_t>::max)() - payout[slot].quantity) {
            return false;
        }
        payout[slot].quantity += policy.quantity;
    }
    report_dismantle_reward_match(
        dismantledDefinition.definitionHash, tier, gearClass, isMasterworked, payoutCount);

    std::int32_t greatestMutationSerial = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        greatestMutationSerial =
            (std::max)(greatestMutationSerial, before.profileItems[index].mutationSerial);
    }

    for (std::size_t policyIndex = 0; policyIndex < payoutCount; ++policyIndex) {
        const DismantleRewardPolicy& policy = payout[policyIndex];
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (policy.definitionHash == authored_inventory::kNoDefinitionHash || policy.quantity <= 0
            || !build_data::find_item_definition_hash(policy.definitionHash, definition)
            || definition.definitionHash != policy.definitionHash
            || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.definitionIndex != definition.definitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
            || detail.maxStackSize <= 0
            || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::profile
            || build_data::is_profile_action_source(definition.definitionIndex,
                                                    definition.bucketId)) {
            return false;
        }

        std::size_t profileIndex = after.profileItemCount;
        for (std::size_t index = 0; index < after.profileItemCount; ++index) {
            const authored_inventory::ProfileItem& item = after.profileItems[index];
            if (item.definitionHash != policy.definitionHash) {
                continue;
            }
            if (item.instanceSoid != 0 || item.quantity <= 0
                || item.quantity > detail.maxStackSize) {
                return false;
            }
            if (profileIndex == after.profileItemCount && item.quantity < detail.maxStackSize) {
                profileIndex = index;
            }
        }

        const bool appended = profileIndex == after.profileItemCount;
        const bool profileFull = appended && after.profileItemCount >= after.profileItems.size();
        if (profileFull || greatestMutationSerial == (std::numeric_limits<std::int32_t>::max)()) {
            report_dismantle_reward_dropped(profileFull ? "profile_full" : "serial_exhausted",
                                            policy.definitionHash,
                                            policy.quantity,
                                            0,
                                            detail.maxStackSize);
            continue;
        }
        const std::int32_t previousQuantity =
            appended ? 0 : after.profileItems[profileIndex].quantity;
        const std::int32_t available = detail.maxStackSize - previousQuantity;
        const std::int32_t credited = (std::min)(policy.quantity, available);
        if (credited <= 0) {
            // Every stack of this currency is already at its native cap.
            report_dismantle_reward_dropped("stack_capped",
                                            policy.definitionHash,
                                            policy.quantity,
                                            previousQuantity,
                                            detail.maxStackSize);
            continue;
        }

        AccountState candidate = after;
        const std::int32_t mutationSerial = greatestMutationSerial + 1;
        const std::int32_t afterQuantity = previousQuantity + credited;
        if (appended) {
            candidate.profileItems[profileIndex] = {
                0, policy.definitionHash, afterQuantity, mutationSerial};
            ++candidate.profileItemCount;
        } else {
            candidate.profileItems[profileIndex].quantity = afterQuantity;
            candidate.profileItems[profileIndex].mutationSerial = mutationSerial;
        }
        // A full native bucket drops this reward, but never blocks deletion of the source item.
        if (!account::valid(candidate) || !valid_profile_inventory(candidate)) {
            report_dismantle_reward_dropped("bucket_full",
                                            policy.definitionHash,
                                            policy.quantity,
                                            previousQuantity,
                                            detail.maxStackSize);
            continue;
        }
        if (rewardCount >= rewards.size()) {
            return false;
        }
        after = candidate;
        greatestMutationSerial = mutationSerial;
        rewards[rewardCount++] = {
            policy.definitionHash, profileIndex, credited, afterQuantity, mutationSerial};
    }
    return account::valid(after) && valid_profile_inventory(after);
}

/**
 * Builds the one canonical dismantle transition for an exact account snapshot.
 *
 * Surviving authored entries keep their mutation generation unless installed row placement moves
 * them. Generation capacity is checked for every move before any survivor is changed.
 */
[[nodiscard]] bool stage_item_dismantle(const AccountState& account,
                                        std::size_t characterIndex,
                                        std::uint64_t instanceSoid,
                                        PendingItemDismantle& mutation) noexcept {
    mutation = {};
    if (instanceSoid == 0 || !account::valid(account) || characterIndex >= account.characterCount
        || !account.characters[characterIndex].selected) {
        return false;
    }

    const CharacterState& before = account.characters[characterIndex];
    std::size_t inventoryIndex = before.inventory.count;
    for (std::size_t index = 0; index < before.inventory.count; ++index) {
        if (before.inventory.values[index].instanceSoid == instanceSoid) {
            inventoryIndex = index;
            break;
        }
    }
    if (inventoryIndex >= before.inventory.count
        || before.inventory.values[inventoryIndex].placement
               != authored_inventory::ItemPlacement::inventory
        || (before.inventory.values[inventoryIndex].flags & authored_inventory::kLockedItemFlag)
               != 0) {
        return false;
    }

    family4_loadout::ResolvedLoadout beforeLoadout{};
    std::uint16_t dismantledRow = 0;
    std::uint8_t dismantledSlot = 0;
    if (!family4_loadout::resolve(account, characterIndex, beforeLoadout)
        || before.nextInventorySerial
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
        || !find_unequipped_row(beforeLoadout, instanceSoid, dismantledRow, dismantledSlot)) {
        return false;
    }

    CharacterState after = before;
    const authored_inventory::Item dismantledItem = after.inventory.values[inventoryIndex];
    for (std::size_t index = inventoryIndex; index + 1U < after.inventory.count; ++index) {
        after.inventory.values[index] = after.inventory.values[index + 1U];
    }
    --after.inventory.count;
    after.inventory.values[after.inventory.count] = {};

    AccountState candidate = account;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout placedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, placedAfter)
        || loadout_contains(placedAfter, instanceSoid)
        || beforeLoadout.itemCount != placedAfter.itemCount + 1U) {
        return false;
    }

    // Survivors keep their serials: the serial is also the Client's bucket ordering token, so a
    // fresh one moves the item to the first cell. The whole character is republished either way.
    std::size_t movedItemCount = 0;
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        const std::uint64_t survivorSoid = after.inventory.values[index].instanceSoid;
        std::uint16_t beforeRow = 0;
        std::uint16_t afterRow = 0;
        std::uint8_t beforeSlot = 0;
        std::uint8_t afterSlot = 0;
        if (!find_unequipped_row(beforeLoadout, survivorSoid, beforeRow, beforeSlot)
            || !find_unequipped_row(placedAfter, survivorSoid, afterRow, afterSlot)
            || beforeSlot != afterSlot) {
            return false;
        }
        movedItemCount += static_cast<std::size_t>(beforeRow != afterRow);
    }

    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout checkedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, checkedAfter)
        || checkedAfter.itemCount != placedAfter.itemCount
        || loadout_contains(checkedAfter, instanceSoid)) {
        return false;
    }
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        const std::uint64_t survivorSoid = after.inventory.values[index].instanceSoid;
        std::uint16_t placedRow = 0;
        std::uint16_t checkedRow = 0;
        std::uint8_t placedSlot = 0;
        std::uint8_t checkedSlot = 0;
        if (!find_unequipped_row(placedAfter, survivorSoid, placedRow, placedSlot)
            || !find_unequipped_row(checkedAfter, survivorSoid, checkedRow, checkedSlot)
            || placedRow != checkedRow || placedSlot != checkedSlot) {
            return false;
        }
    }

    build_data::items::Definition dismantledDefinition{};
    item_details::Definition dismantledDetail{};
    if (!build_data::find_item_definition_hash(dismantledItem.definitionHash, dismantledDefinition)
        || dismantledDefinition.definitionHash != dismantledItem.definitionHash
        || !build_data::find_configured_item_detail(dismantledDefinition.definitionIndex,
                                                    dismantledDetail)
        || dismantledDetail.definitionIndex != dismantledDefinition.definitionIndex
        || dismantledDetail.definitionHash != dismantledDefinition.definitionHash
        || dismantledDetail.bucketId != dismantledDefinition.bucketId
        // A quest step is authored stackable, so a single-unit row is accepted. A larger stack is
        // refused: decrementing one is a different mutation.
        || (dismantledDetail.instancedDefinitionState
                != item_details::InstancedDefinitionState::instanced
            && dismantledItem.quantity != 1)
        // A pursuit names no equipment slot, and the loadout resolver stands it at slot zero.
        // Slot zero maps to no gear class, so a dismantled pursuit pays out nothing.
        || (dismantledDetail.equipmentSlot.has_value()
                ? static_cast<std::uint8_t>(*dismantledDetail.equipmentSlot)
                : std::uint8_t{0})
               != dismantledSlot) {
        return false;
    }

    AccountState rewarded{};
    std::array<DismantleReward, kDismantleRewardCapacity> rewards{};
    std::size_t rewardCount = 0;
    if (!apply_dismantle_rewards(candidate,
                                 dismantledItem,
                                 dismantledDefinition,
                                 dismantledDetail,
                                 dismantledSlot,
                                 rewarded,
                                 rewards,
                                 rewardCount)) {
        return false;
    }
    candidate = rewarded;

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.beforeProfileItems = account.profileItems;
    mutation.afterProfileItems = candidate.profileItems;
    mutation.rewards = rewards;
    mutation.dismantledItem = dismantledItem;
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.dismantledInstanceSoid = instanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.expectedInventoryCount = before.inventory.count;
    mutation.expectedProfileItemCount = account.profileItemCount;
    mutation.afterProfileItemCount = candidate.profileItemCount;
    mutation.inventoryIndex = inventoryIndex;
    mutation.movedInventoryItemCount = movedItemCount;
    mutation.rewardCount = rewardCount;
    mutation.inventoryRow = dismantledRow;
    mutation.equipmentSlot = dismantledSlot;
    mutation.profileChanged = rewardCount != 0;
    mutation.prepared = true;
    return true;
}

/** @return True when both descriptions name the same credited profile mutation. */
[[nodiscard]] bool same_dismantle_reward(const DismantleReward& left,
                                         const DismantleReward& right) noexcept {
    return left.definitionHash == right.definitionHash && left.profileIndex == right.profileIndex
           && left.quantity == right.quantity && left.afterQuantity == right.afterQuantity
           && left.mutationSerial == right.mutationSerial;
}

/** @return True when two independently staged dismantles carry the exact same after-images. */
[[nodiscard]] bool same_dismantle_transition(const PendingItemDismantle& left,
                                             const PendingItemDismantle& right) noexcept {
    if (left.prepared != right.prepared || left.accountSoid != right.accountSoid
        || left.characterSoid != right.characterSoid
        || left.dismantledInstanceSoid != right.dismantledInstanceSoid
        || left.characterIndex != right.characterIndex
        || left.expectedInventoryCount != right.expectedInventoryCount
        || left.expectedProfileItemCount != right.expectedProfileItemCount
        || left.afterProfileItemCount != right.afterProfileItemCount
        || left.inventoryIndex != right.inventoryIndex
        || left.movedInventoryItemCount != right.movedInventoryItemCount
        || left.rewardCount != right.rewardCount || left.inventoryRow != right.inventoryRow
        || left.equipmentSlot != right.equipmentSlot || left.profileChanged != right.profileChanged
        || !same_stationary_item(left.dismantledItem, right.dismantledItem)
        || !same_character(left.beforeCharacter, right.beforeCharacter)
        || !same_character(left.afterCharacter, right.afterCharacter)
        || !same_profile_views(left.beforeProfileItems,
                               left.expectedProfileItemCount,
                               right.beforeProfileItems,
                               right.expectedProfileItemCount)
        || !same_profile_views(left.afterProfileItems,
                               left.afterProfileItemCount,
                               right.afterProfileItems,
                               right.afterProfileItemCount)) {
        return false;
    }
    for (std::size_t index = 0; index < left.rewards.size(); ++index) {
        if (!same_dismantle_reward(left.rewards[index], right.rewards[index])) {
            return false;
        }
    }
    return true;
}

/** Applies a fully checked dismantle after-image over an exact current account view. */
[[nodiscard]] bool materialize_item_dismantle(const AccountState& current,
                                              const PendingItemDismantle& mutation,
                                              AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.dismantledInstanceSoid == 0
        || mutation.dismantledItem.instanceSoid != mutation.dismantledInstanceSoid
        || mutation.dismantledItem.definitionHash == authored_inventory::kNoDefinitionHash
        || mutation.characterIndex >= current.characterCount || mutation.expectedInventoryCount == 0
        || mutation.expectedInventoryCount > authored_inventory::kCharacterItemCapacity
        || mutation.expectedProfileItemCount > authored_inventory::kProfileItemCapacity
        || mutation.afterProfileItemCount > authored_inventory::kProfileItemCapacity
        || mutation.inventoryIndex >= mutation.expectedInventoryCount
        || mutation.rewardCount > mutation.rewards.size()
        || mutation.profileChanged != (mutation.rewardCount != 0)
        || mutation.beforeCharacter.soid != mutation.characterSoid
        || mutation.afterCharacter.soid != mutation.characterSoid
        || mutation.beforeCharacter.inventory.count != mutation.expectedInventoryCount
        || mutation.afterCharacter.inventory.count + 1U != mutation.expectedInventoryCount
        || !same_stationary_item(mutation.beforeCharacter.inventory.values[mutation.inventoryIndex],
                                 mutation.dismantledItem)
        || current.primarySoid != mutation.accountSoid
        || !same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.expectedProfileItemCount)) {
        return false;
    }
    const CharacterState& character = current.characters[mutation.characterIndex];
    if (!character.selected || character.soid != mutation.characterSoid
        || !same_character(character, mutation.beforeCharacter)) {
        return false;
    }
    for (std::size_t index = 0; index < mutation.rewards.size(); ++index) {
        const DismantleReward& reward = mutation.rewards[index];
        if (index < mutation.rewardCount) {
            if (reward.definitionHash == authored_inventory::kNoDefinitionHash
                || reward.profileIndex >= mutation.afterProfileItemCount || reward.quantity <= 0
                || reward.afterQuantity < reward.quantity || reward.mutationSerial <= 0) {
                return false;
            }
            const authored_inventory::ProfileItem& row =
                mutation.afterProfileItems[reward.profileIndex];
            if (row.instanceSoid != 0 || row.definitionHash != reward.definitionHash
                || row.quantity != reward.afterQuantity
                || row.mutationSerial != reward.mutationSerial) {
                return false;
            }
        } else if (reward.definitionHash != 0 || reward.profileIndex != 0 || reward.quantity != 0
                   || reward.afterQuantity != 0 || reward.mutationSerial != 0) {
            return false;
        }
    }
    if (!mutation.profileChanged
        && !same_profile_views(mutation.beforeProfileItems,
                               mutation.expectedProfileItemCount,
                               mutation.afterProfileItems,
                               mutation.afterProfileItemCount)) {
        return false;
    }

    PendingItemDismantle canonical{};
    if (!stage_item_dismantle(
            current, mutation.characterIndex, mutation.dismantledInstanceSoid, canonical)
        || !same_dismantle_transition(canonical, mutation)) {
        return false;
    }

    after = current;
    after.characters[mutation.characterIndex] = mutation.afterCharacter;
    after.profileItems = mutation.afterProfileItems;
    after.profileItemCount = mutation.afterProfileItemCount;
    return account::valid(after) && valid_profile_inventory(after)
           && !identity_uses_soid(after, mutation.dismantledInstanceSoid);
}

} // namespace runtime::detail
} // namespace sunrise::state
