#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace family4_loadout = middleware::datagen::family4::loadout;

/** Prepares one checked ordinary-socket selection without publishing account State. */
bool prepare_socket_plug(std::uint64_t targetInstanceSoid,
                         std::uint8_t socketLane,
                         std::uint16_t plugDefinitionIndex,
                         std::int64_t nowSeconds,
                         PendingSocketPlug& mutation) noexcept {
    mutation = {};
    const AccountState snapshot = bound_account_snapshot();
    std::size_t characterIndex = snapshot.characterCount;
    if (targetInstanceSoid != 0 && account::valid(snapshot)) {
        for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
            if (snapshot.characters[index].selected) {
                characterIndex = index;
                break;
            }
        }
    }
    if (characterIndex >= snapshot.characterCount
        || !stage_socket_plug(snapshot,
                              characterIndex,
                              targetInstanceSoid,
                              socketLane,
                              plugDefinitionIndex,
                              nowSeconds,
                              mutation)) {
        report_socket_plug("prepare",
                           "fail",
                           "ownership_definition_or_compatibility",
                           0,
                           targetInstanceSoid,
                           0,
                           socketLane,
                           plugDefinitionIndex,
                           0,
                           0,
                           false,
                           0);
        return false;
    }
    report_socket_plug("prepare",
                       "ok",
                       "ready",
                       mutation.characterSoid,
                       mutation.targetInstanceSoid,
                       mutation.targetDefinitionIndex,
                       mutation.socketLane,
                       mutation.plugDefinitionIndex,
                       mutation.targetBucketId,
                       mutation.plugBucketId,
                       mutation.targetEquipped,
                       mutation.itemIndex);
    return true;
}

/** Prepares a socket transition for the whole instance identity carried by opcode 1901. */
bool prepare_equipped_socket_plug(std::uint64_t targetInstanceSoid,
                                  std::uint8_t requestedSocketLane,
                                  std::uint16_t plugDefinitionIndex,
                                  std::int64_t nowSeconds,
                                  PendingSocketPlug& mutation) noexcept {
    mutation = {};
    const AccountState snapshot = bound_account_snapshot();
    std::size_t characterIndex = snapshot.characterCount;
    if (account::valid(snapshot)) {
        for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
            if (snapshot.characters[index].selected) {
                characterIndex = index;
                break;
            }
        }
    }

    if (characterIndex >= snapshot.characterCount || targetInstanceSoid == 0
        || requestedSocketLane >= authored_inventory::kPlugCapacity) {
        report_socket_plug("prepare_equipped",
                           "fail",
                           "selection_or_slot",
                           0,
                           targetInstanceSoid,
                           0,
                           requestedSocketLane,
                           plugDefinitionIndex,
                           0,
                           0,
                           false,
                           0);
        return false;
    }

    family4_loadout::ResolvedLoadout resolvedLoadout{};
    if (!family4_loadout::resolve(snapshot, characterIndex, resolvedLoadout)) {
        return false;
    }
    const family4_loadout::ResolvedItem* resolvedTarget = nullptr;
    for (std::size_t index = 0; index < resolvedLoadout.itemCount; ++index) {
        const family4_loadout::ResolvedItem& item = resolvedLoadout.items[index];
        if (item.instance.instanceSoid != targetInstanceSoid) {
            continue;
        }
        if (resolvedTarget != nullptr) {
            return false;
        }
        resolvedTarget = &item;
    }
    const bool expectedEquipped = resolvedTarget != nullptr && resolvedTarget->equipped;

    std::uint8_t resolvedSocketLane = static_cast<std::uint8_t>(authored_inventory::kPlugCapacity);
    build_data::items::Definition targetDefinition{};
    item_details::Definition targetDetail{};
    bool ambiguousSocketLane = false;
    if (resolvedTarget != nullptr
        && build_data::find_item_definition_index(resolvedTarget->instance.baseDefinitionIndex,
                                                  targetDefinition)
        && targetDefinition.definitionIndex == resolvedTarget->instance.baseDefinitionIndex
        && build_data::find_configured_item_detail(targetDefinition.definitionIndex, targetDetail)
        && targetDetail.definitionIndex == targetDefinition.definitionIndex
        && targetDetail.definitionHash == targetDefinition.definitionHash
        && targetDetail.bucketId == targetDefinition.bucketId
        && targetDetail.ordinarySocketState == item_details::OrdinarySocketState::present
        && targetDetail.ordinarySocketCount <= authored_inventory::kPlugCapacity) {
        // Most action kinds name the physical lane, so prefer it when its pool accepts the plug.
        // Shaders are semantic instead, so the unique-compatible-lane fallback stays.
        if (requestedSocketLane < targetDetail.ordinarySocketCount
            && build_data::is_socket_plug_allowed(
                targetDefinition.definitionIndex, requestedSocketLane, plugDefinitionIndex)) {
            resolvedSocketLane = requestedSocketLane;
        } else {
            for (std::size_t lane = 0; lane < targetDetail.ordinarySocketCount; ++lane) {
                if (!build_data::is_socket_plug_allowed(targetDefinition.definitionIndex,
                                                        static_cast<std::uint8_t>(lane),
                                                        plugDefinitionIndex)) {
                    continue;
                }
                if (resolvedSocketLane < authored_inventory::kPlugCapacity) {
                    ambiguousSocketLane = true;
                    break;
                }
                resolvedSocketLane = static_cast<std::uint8_t>(lane);
            }
        }
    }
    if (resolvedTarget == nullptr || resolvedTarget->instance.instanceSoid == 0
        || ambiguousSocketLane || resolvedSocketLane >= authored_inventory::kPlugCapacity
        || !stage_socket_plug(snapshot,
                              characterIndex,
                              resolvedTarget->instance.instanceSoid,
                              resolvedSocketLane,
                              plugDefinitionIndex,
                              nowSeconds,
                              mutation)
        || mutation.targetEquipped != expectedEquipped) {
        report_socket_plug("prepare_equipped",
                           "fail",
                           "equipment_or_compatibility",
                           snapshot.characters[characterIndex].soid,
                           targetInstanceSoid,
                           0,
                           resolvedSocketLane,
                           plugDefinitionIndex,
                           0,
                           0,
                           expectedEquipped,
                           0);
        mutation = {};
        return false;
    }

    report_socket_plug("prepare_equipped",
                       "ok",
                       "ready",
                       mutation.characterSoid,
                       mutation.targetInstanceSoid,
                       mutation.targetDefinitionIndex,
                       mutation.socketLane,
                       mutation.plugDefinitionIndex,
                       mutation.targetBucketId,
                       mutation.plugBucketId,
                       mutation.targetEquipped,
                       mutation.itemIndex);
    return true;
}

/** Applies a socket action's unlock writes, whose rows the catalogs bound, to the banks. */
static void apply_unlock_writes(const AccountUnlockWrites& writes, unlocks::Table& banks) noexcept {
    for (const AccountUnlockWrite& write : std::span(writes.rows).first(writes.count)) {
        if (write.bank == AccountUnlockBank::flags) {
            banks.accountFlags[write.index] = static_cast<std::uint8_t>(write.value);
        } else {
            banks.objectiveValues[write.index] = write.value;
        }
    }
}

/**
 * @return True when two stagings of one socket request reached the same outcome. Their profile
 *         rows are left to the caller, which checks them against its own account view.
 */
[[nodiscard]] static bool same_socket_transition(const PendingSocketPlug& left,
                                                 const PendingSocketPlug& right) noexcept {
    return left.accountSoid == right.accountSoid && left.characterSoid == right.characterSoid
           && left.targetDefinitionHash == right.targetDefinitionHash
           && left.plugDefinitionHash == right.plugDefinitionHash
           && left.materialRequirementSetHash == right.materialRequirementSetHash
           && left.characterIndex == right.characterIndex
           && left.expectedProfileItemCount == right.expectedProfileItemCount
           && left.afterProfileItemCount == right.afterProfileItemCount
           && left.changes == right.changes && left.changeCount == right.changeCount
           && left.unlockWrites == right.unlockWrites && left.itemIndex == right.itemIndex
           && left.targetDefinitionIndex == right.targetDefinitionIndex
           && left.plugDefinitionIndex == right.plugDefinitionIndex
           && left.requestedPlugDefinitionIndex == right.requestedPlugDefinitionIndex
           && left.materialRequirementSetIndex == right.materialRequirementSetIndex
           && left.socketLane == right.socketLane && left.targetBucketId == right.targetBucketId
           && left.plugBucketId == right.plugBucketId
           && left.materialRequirementCount == right.materialRequirementCount
           && left.profileChanged == right.profileChanged
           && left.targetEquipped == right.targetEquipped
           && same_character(left.beforeCharacter, right.beforeCharacter)
           && same_character(left.afterCharacter, right.afterCharacter);
}

/** Produces the complete account after-image while the prepared socket action remains current. */
bool preview_socket_plug(const PendingSocketPlug& mutation,
                         AccountState& after,
                         unlocks::Table& afterUnlocks) noexcept {
    after = {};
    afterUnlocks = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.targetInstanceSoid == 0 || mutation.characterIndex >= kCharacterCapacity
        || mutation.expectedProfileItemCount > authored_inventory::kProfileItemCapacity
        || mutation.afterProfileItemCount > authored_inventory::kProfileItemCapacity) {
        return false;
    }
    // The account and the unlock rows the action reads come from one locked save view.
    const std::lock_guard lock(investment::store::g_mutex);
    const AccountState current = bound_account_snapshot();
    if (mutation.characterIndex >= current.characterCount
        || current.primarySoid != mutation.accountSoid
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.expectedProfileItemCount)) {
        return false;
    }

    PendingSocketPlug canonical{};
    if (!stage_socket_plug(current,
                           mutation.characterIndex,
                           mutation.targetInstanceSoid,
                           mutation.socketLane,
                           mutation.requestedPlugDefinitionIndex,
                           mutation.nowSeconds,
                           canonical,
                           mutation.plugDefinitionHash)
        || !same_socket_transition(canonical, mutation)) {
        return false;
    }

    if (!investment::store::read_unlocks(afterUnlocks,
                                         static_cast<int>(mutation.characterIndex))) {
        return false;
    }
    apply_unlock_writes(mutation.unlockWrites, afterUnlocks);
    after = current;
    after.profileItems = canonical.afterProfileItems;
    after.profileItemCount = canonical.afterProfileItemCount;
    after.characters[mutation.characterIndex] = canonical.afterCharacter;
    const bool accountChanged = mutation.unlockWrites.count != 0
                                || !same_profile_inventory(after,
                                                           mutation.beforeProfileItems,
                                                           mutation.expectedProfileItemCount);
    family4_loadout::ResolvedLoadout resolved{};
    return accountChanged == mutation.profileChanged
           && same_profile_inventory(
               after, mutation.afterProfileItems, mutation.afterProfileItemCount)
           && account::valid(after) && valid_profile_inventory(after)
           && family4_loadout::resolve(after, mutation.characterIndex, resolved);
}

/** Commits one prepared socket-and-material after-image behind exact staleness guards. */
bool commit_socket_plug(PendingSocketPlug& mutation) noexcept {
    const PendingSocketPlug& prepared = mutation;
    const PendingConsumption consume{mutation};
    const auto fail = [&prepared](std::string_view reason) noexcept {
        report_socket_plug("commit",
                           "fail",
                           reason,
                           prepared.characterSoid,
                           prepared.targetInstanceSoid,
                           prepared.targetDefinitionIndex,
                           prepared.socketLane,
                           prepared.plugDefinitionIndex,
                           prepared.targetBucketId,
                           prepared.plugBucketId,
                           prepared.targetEquipped,
                           prepared.itemIndex);
        return false;
    };
    const std::size_t itemCapacity = prepared.targetEquipped
                                         ? authored_inventory::kEquipmentSlotCount
                                         : authored_inventory::kCharacterItemCapacity;
    if (!prepared.prepared || prepared.accountSoid == 0 || prepared.characterSoid == 0
        || prepared.targetInstanceSoid == 0
        || prepared.targetDefinitionHash == authored_inventory::kNoDefinitionHash
        || prepared.plugDefinitionHash == authored_inventory::kNoDefinitionHash
        || prepared.characterIndex >= kCharacterCapacity || prepared.itemIndex >= itemCapacity
        || prepared.expectedProfileItemCount > authored_inventory::kProfileItemCapacity
        || prepared.afterProfileItemCount > authored_inventory::kProfileItemCapacity
        || prepared.socketLane >= authored_inventory::kPlugCapacity
        || prepared.beforeCharacter.soid != prepared.characterSoid
        || prepared.afterCharacter.soid != prepared.characterSoid) {
        return fail("mutation");
    }

    report_socket_plug("commit_begin",
                       "ok",
                       "ready",
                       prepared.characterSoid,
                       prepared.targetInstanceSoid,
                       prepared.targetDefinitionIndex,
                       prepared.socketLane,
                       prepared.plugDefinitionIndex,
                       prepared.targetBucketId,
                       prepared.plugBucketId,
                       prepared.targetEquipped,
                       prepared.itemIndex);

    {
        investment::store::Transaction transaction;
        if (!transaction.ready()) {
            return fail("transaction");
        }
        AccountState candidate = investment::store::account();
        if (prepared.characterIndex >= candidate.characterCount
            || candidate.primarySoid != prepared.accountSoid
            || !same_profile_inventory(
                candidate, prepared.beforeProfileItems, prepared.expectedProfileItemCount)
            || !same_character(candidate.characters[prepared.characterIndex],
                               prepared.beforeCharacter)) {
            return fail("stale");
        }

        // Re-staging reads every unlock row the action writes, so an equal transition proves
        // each row still holds the value the preparation observed.
        PendingSocketPlug canonical{};
        if (!stage_socket_plug(candidate,
                               prepared.characterIndex,
                               prepared.targetInstanceSoid,
                               prepared.socketLane,
                               prepared.requestedPlugDefinitionIndex,
                               prepared.nowSeconds,
                               canonical,
                               prepared.plugDefinitionHash)
            || !same_socket_transition(canonical, prepared)) {
            return fail("transition");
        }

        candidate.profileItems = canonical.afterProfileItems;
        candidate.profileItemCount = canonical.afterProfileItemCount;
        if (!same_profile_inventory(
                candidate, prepared.afterProfileItems, prepared.afterProfileItemCount)) {
            return fail("materials");
        }
        candidate.characters[prepared.characterIndex] = canonical.afterCharacter;
        family4_loadout::ResolvedLoadout checked{};
        if (!account::valid(candidate) || !valid_profile_inventory(candidate)
            || !family4_loadout::resolve(candidate, prepared.characterIndex, checked)) {
            return fail("account_or_resolve");
        }
        if (!investment::store::write_account(candidate)) {
            return fail("account_write");
        }
        for (const AccountUnlockWrite& write :
             std::span(prepared.unlockWrites.rows).first(prepared.unlockWrites.count)) {
            if (!investment::store::write_unlock(
                    store_bank(write.bank), write.index, write.value)) {
                return fail("unlock_write");
            }
        }
        if (!transaction.commit()) {
            return fail("transaction_commit");
        }
    }

    report_socket_plug("commit_end",
                       "ok",
                       "published",
                       prepared.characterSoid,
                       prepared.targetInstanceSoid,
                       prepared.targetDefinitionIndex,
                       prepared.socketLane,
                       prepared.plugDefinitionIndex,
                       prepared.targetBucketId,
                       prepared.plugBucketId,
                       prepared.targetEquipped,
                       prepared.itemIndex);
    return true;
}

/** Prepares one complete native item-state value for an owned selected-character instance. */
bool prepare_item_state(std::uint64_t targetInstanceSoid,
                        std::uint16_t targetDefinitionIndex,
                        std::uint32_t flags,
                        PendingItemState& mutation) noexcept {
    mutation = {};
    const AccountState snapshot = bound_account_snapshot();
    std::size_t characterIndex = snapshot.characterCount;
    if (account::valid(snapshot)) {
        for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
            if (snapshot.characters[index].selected) {
                characterIndex = index;
                break;
            }
        }
    }
    if (characterIndex >= snapshot.characterCount
        || !stage_item_state(
            snapshot, characterIndex, targetInstanceSoid, targetDefinitionIndex, flags, mutation)) {
        report_item_state(
            "prepare",
            "fail",
            "selection_or_item",
            characterIndex < snapshot.characterCount ? snapshot.characters[characterIndex].soid : 0,
            targetInstanceSoid,
            targetDefinitionIndex,
            0,
            flags,
            false,
            0);
        mutation = {};
        return false;
    }
    report_item_state("prepare",
                      "ok",
                      "ready",
                      mutation.characterSoid,
                      mutation.targetInstanceSoid,
                      mutation.targetDefinitionIndex,
                      mutation.beforeFlags,
                      mutation.afterFlags,
                      mutation.targetEquipped,
                      mutation.itemIndex);
    return true;
}

/** Commits one prepared item-state after-image behind an exact character staleness guard. */
bool commit_item_state(PendingItemState& mutation) noexcept {
    const PendingItemState& prepared = mutation;
    const PendingConsumption consume{mutation};
    const auto fail = [&prepared](std::string_view reason) noexcept {
        report_item_state("commit",
                          "fail",
                          reason,
                          prepared.characterSoid,
                          prepared.targetInstanceSoid,
                          prepared.targetDefinitionIndex,
                          prepared.beforeFlags,
                          prepared.afterFlags,
                          prepared.targetEquipped,
                          prepared.itemIndex);
        return false;
    };
    const std::size_t capacity = prepared.targetEquipped
                                     ? authored_inventory::kEquipmentSlotCount
                                     : authored_inventory::kCharacterItemCapacity;
    if (!prepared.prepared || prepared.characterSoid == 0 || prepared.targetInstanceSoid == 0
        || prepared.characterIndex >= kCharacterCapacity || prepared.itemIndex >= capacity
        || prepared.beforeCharacter.soid != prepared.characterSoid
        || prepared.afterCharacter.soid != prepared.characterSoid
        || prepared.beforeFlags == prepared.afterFlags) {
        return fail("mutation");
    }

    investment::store::g_mutex.lock();
    AccountState candidate = investment::store::account();
    if (prepared.characterIndex >= candidate.characterCount
        || !same_character(candidate.characters[prepared.characterIndex],
                           prepared.beforeCharacter)) {
        investment::store::g_mutex.unlock();
        return fail("stale");
    }

    PendingItemState canonical{};
    if (!stage_item_state(candidate,
                          prepared.characterIndex,
                          prepared.targetInstanceSoid,
                          prepared.targetDefinitionIndex,
                          prepared.afterFlags,
                          canonical)
        || canonical.characterSoid != prepared.characterSoid
        || canonical.characterIndex != prepared.characterIndex
        || canonical.itemIndex != prepared.itemIndex
        || canonical.targetDefinitionIndex != prepared.targetDefinitionIndex
        || canonical.beforeFlags != prepared.beforeFlags
        || canonical.afterFlags != prepared.afterFlags
        || canonical.targetEquipped != prepared.targetEquipped
        || !same_character(canonical.beforeCharacter, prepared.beforeCharacter)
        || !same_character(canonical.afterCharacter, prepared.afterCharacter)) {
        investment::store::g_mutex.unlock();
        return fail("transition");
    }

    candidate.characters[prepared.characterIndex] = prepared.afterCharacter;
    family4_loadout::ResolvedLoadout checked{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, prepared.characterIndex, checked)) {
        investment::store::g_mutex.unlock();
        return fail("account_or_resolve");
    }
    if (!investment::store::write_account(candidate)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    investment::store::g_mutex.unlock();

    report_item_state("commit",
                      "ok",
                      "published",
                      prepared.characterSoid,
                      prepared.targetInstanceSoid,
                      prepared.targetDefinitionIndex,
                      prepared.beforeFlags,
                      prepared.afterFlags,
                      prepared.targetEquipped,
                      prepared.itemIndex);
    return true;
}

/** Prepares one checked subclass socket-entry selection without publishing account State. */
bool prepare_subclass_selection(std::uint64_t subclassInstanceSoid,
                                std::uint8_t requestedEntry,
                                PendingSubclassSelection& mutation) noexcept {
    mutation = {};
    const AccountState snapshot = bound_account_snapshot();
    std::size_t characterIndex = snapshot.characterCount;
    if (account::valid(snapshot)) {
        for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
            if (snapshot.characters[index].selected) {
                characterIndex = index;
                break;
            }
        }
    }
    if (characterIndex >= snapshot.characterCount
        || !stage_subclass_selection(
            snapshot, characterIndex, subclassInstanceSoid, requestedEntry, mutation)) {
        mutation = {};
        return false;
    }
    return true;
}

/** Produces the complete account after-image while the prepared subclass action remains current. */
bool preview_subclass_selection(const PendingSubclassSelection& mutation,
                                AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.subclassInstanceSoid == 0 || mutation.characterIndex >= kCharacterCapacity) {
        return false;
    }
    const AccountState current = bound_account_snapshot();
    if (mutation.characterIndex >= current.characterCount
        || current.primarySoid != mutation.accountSoid
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)) {
        return false;
    }
    PendingSubclassSelection canonical{};
    if (!stage_subclass_selection(current,
                                  mutation.characterIndex,
                                  mutation.subclassInstanceSoid,
                                  mutation.requestedEntry,
                                  canonical)
        || !same_character(canonical.afterCharacter, mutation.afterCharacter)) {
        return false;
    }
    after = current;
    after.characters[mutation.characterIndex] = canonical.afterCharacter;
    family4_loadout::ResolvedLoadout resolved{};
    return account::valid(after)
           && family4_loadout::resolve(after, mutation.characterIndex, resolved);
}

/** Commits one prepared subclass selection behind exact account and character guards. */
bool commit_subclass_selection(PendingSubclassSelection& mutation) noexcept {
    const PendingSubclassSelection& prepared = mutation;
    const PendingConsumption consume{mutation};
    if (!prepared.prepared || prepared.accountSoid == 0 || prepared.characterSoid == 0
        || prepared.subclassInstanceSoid == 0 || prepared.characterIndex >= kCharacterCapacity
        || prepared.beforeCharacter.soid != prepared.characterSoid
        || prepared.afterCharacter.soid != prepared.characterSoid) {
        return false;
    }

    investment::store::g_mutex.lock();
    AccountState candidate = investment::store::account();
    if (prepared.characterIndex >= candidate.characterCount
        || candidate.primarySoid != prepared.accountSoid
        || !same_character(candidate.characters[prepared.characterIndex],
                           prepared.beforeCharacter)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    PendingSubclassSelection canonical{};
    if (!stage_subclass_selection(candidate,
                                  prepared.characterIndex,
                                  prepared.subclassInstanceSoid,
                                  prepared.requestedEntry,
                                  canonical)
        || !same_character(canonical.afterCharacter, prepared.afterCharacter)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    candidate.characters[prepared.characterIndex] = canonical.afterCharacter;
    family4_loadout::ResolvedLoadout checked{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, prepared.characterIndex, checked)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    if (!investment::store::write_account(candidate)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    investment::store::g_mutex.unlock();

    // The published ability buckets are keyed to whichever selection is currently active; that
    // just changed, so the domain is stale the moment the account write above becomes visible.
    build_data::invalidate_ability_buckets();
    return true;
}

} // namespace sunrise::state
