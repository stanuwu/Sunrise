#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;
namespace family4_loadout = middleware::datagen::family4::loadout;

/** Prepares one native-row-checked selected-character inventory insertion. */
bool prepare_item_acquisition(std::uint16_t collectibleIndex,
                              std::uint32_t definitionHash,
                              PendingItemAcquisition& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    build_data::collectibles::Definition collectible{};
    build_data::items::Definition grantedDefinition{};
    if (definitionHash == authored_inventory::kNoDefinitionHash || !account::valid(account)
        || !valid_profile_inventory(account)
        || !build_data::find_collectible_definition(collectibleIndex, collectible)
        || collectible.itemDefinitionIndex
               == build_data::collectibles::kUnavailableItemDefinitionIndex
        || !build_data::find_item_definition_index(collectible.itemDefinitionIndex,
                                                   grantedDefinition)
        || grantedDefinition.definitionHash != definitionHash) {
        report_acquisition("prepare", "fail", "input", definitionHash, 0, 0, 0, 0, 0, 0);
        return false;
    }

    AccountState chargedAccount{};
    bool profileChanged = false;
    if (!apply_collection_materials(account, collectible, chargedAccount, profileChanged)) {
        report_acquisition("prepare", "fail", "materials", definitionHash, 0, 0, 0, 0, 0, 0);
        return false;
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex == account.characterCount) {
        report_acquisition("prepare", "fail", "selection", definitionHash, 0, 0, 0, 0, 0, 0);
        return false;
    }

    const CharacterState& before = account.characters[characterIndex];
    if (before.inventory.count >= before.inventory.values.size()
        || before.nextInventorySerial
               >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        report_acquisition("prepare",
                           "fail",
                           "state_capacity",
                           definitionHash,
                           before.soid,
                           0,
                           before.inventory.count,
                           0,
                           0,
                           before.nextInventorySerial);
        return false;
    }

    std::uint64_t instanceSoid = 0;
    if (!next_item_instance_soid(account, instanceSoid)) {
        report_acquisition("prepare",
                           "fail",
                           "soid",
                           definitionHash,
                           before.soid,
                           0,
                           before.inventory.count,
                           0,
                           0,
                           before.nextInventorySerial);
        return false;
    }

    CharacterState after = before;
    const std::size_t inventoryIndex = after.inventory.count;
    authored_inventory::Item acquired{};
    acquired.instanceSoid = instanceSoid;
    acquired.definitionHash = definitionHash;
    acquired.level = acquisition_level(before);
    acquired.quantity = 1;
    acquired.mutationSerial = static_cast<std::int32_t>(after.nextInventorySerial++);
    acquired.sockets.policy = authored_inventory::SocketPolicy::nativeDefaults;
    after.inventory.values[inventoryIndex] = acquired;
    ++after.inventory.count;

    AccountState candidate = chargedAccount;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout resolved{};
    std::uint16_t inventoryRow = 0;
    std::uint8_t equipmentSlot = 0;
    if (!account::valid(candidate) || identity_uses_soid(candidate, instanceSoid)
        || !family4_loadout::resolve(candidate, characterIndex, resolved)
        || !find_acquired_row(resolved, instanceSoid, inventoryRow, equipmentSlot)) {
        report_acquisition("prepare",
                           "fail",
                           "resolve_or_bucket_full",
                           definitionHash,
                           before.soid,
                           instanceSoid,
                           inventoryIndex,
                           0,
                           0,
                           after.nextInventorySerial);
        return false;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.beforeProfileItems = account.profileItems;
    mutation.afterProfileItems = chargedAccount.profileItems;
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.acquiredInstanceSoid = instanceSoid;
    mutation.acquiredDefinitionHash = definitionHash;
    mutation.materialRequirementSetHash = collectible.materialRequirementSetHash;
    mutation.expectedNextInventorySerial = before.nextInventorySerial;
    mutation.characterIndex = characterIndex;
    mutation.expectedInventoryCount = before.inventory.count;
    mutation.expectedProfileItemCount = account.profileItemCount;
    mutation.afterProfileItemCount = chargedAccount.profileItemCount;
    mutation.inventoryIndex = inventoryIndex;
    mutation.collectibleIndex = collectibleIndex;
    mutation.inventoryRow = inventoryRow;
    mutation.equipmentSlot = equipmentSlot;
    mutation.materialRequirementCount = collectible.materialRequirementCount;
    mutation.profileChanged = profileChanged;
    mutation.prepared = true;
    report_acquisition("prepare",
                       "ok",
                       "ready",
                       definitionHash,
                       before.soid,
                       instanceSoid,
                       inventoryIndex,
                       inventoryRow,
                       equipmentSlot,
                       after.nextInventorySerial);
    return true;
}

/** Produces the full account after-image while a prepared character pull remains current. */
bool preview_item_acquisition(const PendingItemAcquisition& mutation,
                              AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.acquiredInstanceSoid == 0 || mutation.characterIndex >= kCharacterCapacity
        || mutation.expectedProfileItemCount > authored_inventory::kProfileItemCapacity
        || mutation.afterProfileItemCount > authored_inventory::kProfileItemCapacity) {
        return false;
    }
    const AccountState current = account_snapshot();
    if (mutation.characterIndex >= current.characterCount
        || current.primarySoid != mutation.accountSoid
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.expectedProfileItemCount)) {
        return false;
    }
    after = current;
    after.profileItems = mutation.afterProfileItems;
    after.profileItemCount = mutation.afterProfileItemCount;
    after.characters[mutation.characterIndex] = mutation.afterCharacter;
    family4_loadout::ResolvedLoadout resolved{};
    std::uint16_t checkedRow = 0;
    std::uint8_t checkedSlot = 0;
    return account::valid(after) && valid_profile_inventory(after)
           && family4_loadout::resolve(after, mutation.characterIndex, resolved)
           && find_acquired_row(resolved, mutation.acquiredInstanceSoid, checkedRow, checkedSlot)
           && checkedRow == mutation.inventoryRow && checkedSlot == mutation.equipmentSlot;
}

/** Commits one prepared insertion only while its prepare-time loadout remains current. */
bool commit_item_acquisition(PendingItemAcquisition& mutation) noexcept {
    const PendingItemAcquisition prepared = mutation;
    mutation = {};
    const auto fail = [&prepared](std::string_view reason) noexcept {
        report_acquisition("commit",
                           "fail",
                           reason,
                           prepared.acquiredDefinitionHash,
                           prepared.characterSoid,
                           prepared.acquiredInstanceSoid,
                           prepared.inventoryIndex,
                           prepared.inventoryRow,
                           prepared.equipmentSlot,
                           prepared.afterCharacter.nextInventorySerial);
        return false;
    };
    if (!prepared.prepared || prepared.characterSoid == 0 || prepared.acquiredInstanceSoid == 0
        || prepared.accountSoid == 0
        || prepared.acquiredDefinitionHash == authored_inventory::kNoDefinitionHash
        || prepared.characterIndex >= kCharacterCapacity
        || prepared.expectedInventoryCount >= authored_inventory::kCharacterItemCapacity
        || prepared.inventoryIndex != prepared.expectedInventoryCount
        || prepared.afterCharacter.inventory.count != prepared.expectedInventoryCount + 1U
        || prepared.inventoryIndex >= prepared.afterCharacter.inventory.count
        || prepared.afterCharacter.inventory.values[prepared.inventoryIndex].instanceSoid
               != prepared.acquiredInstanceSoid
        || prepared.afterCharacter.inventory.values[prepared.inventoryIndex].definitionHash
               != prepared.acquiredDefinitionHash
        || prepared.expectedProfileItemCount > authored_inventory::kProfileItemCapacity
        || prepared.afterProfileItemCount > authored_inventory::kProfileItemCapacity) {
        return fail("mutation");
    }

    build_data::collectibles::Definition collectible{};
    if (!build_data::find_collectible_definition(prepared.collectibleIndex, collectible)
        || collectible.itemDefinitionIndex
               == build_data::collectibles::kUnavailableItemDefinitionIndex
        || collectible.materialRequirementSetHash != prepared.materialRequirementSetHash
        || collectible.materialRequirementCount != prepared.materialRequirementCount) {
        return fail("collectible");
    }

    report_acquisition("commit_begin",
                       "ok",
                       "ready",
                       prepared.acquiredDefinitionHash,
                       prepared.characterSoid,
                       prepared.acquiredInstanceSoid,
                       prepared.inventoryIndex,
                       prepared.inventoryRow,
                       prepared.equipmentSlot,
                       prepared.afterCharacter.nextInventorySerial);

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (prepared.characterIndex >= candidate.characterCount
        || candidate.primarySoid != prepared.accountSoid
        || !same_profile_inventory(
            candidate, prepared.beforeProfileItems, prepared.expectedProfileItemCount)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return fail("account");
    }
    CharacterState& character = candidate.characters[prepared.characterIndex];
    if (!character.selected || character.soid != prepared.characterSoid
        || !same_character(character, prepared.beforeCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return fail("stale");
    }

    candidate.profileItems = prepared.afterProfileItems;
    candidate.profileItemCount = prepared.afterProfileItemCount;
    character = prepared.afterCharacter;
    family4_loadout::ResolvedLoadout resolved{};
    std::uint16_t checkedRow = 0;
    std::uint8_t checkedSlot = 0;
    if (!account::valid(candidate) || !valid_profile_inventory(candidate)
        || identity_uses_soid(candidate, prepared.acquiredInstanceSoid)
        || !family4_loadout::resolve(candidate, prepared.characterIndex, resolved)
        || !find_acquired_row(resolved, prepared.acquiredInstanceSoid, checkedRow, checkedSlot)
        || checkedRow != prepared.inventoryRow || checkedSlot != prepared.equipmentSlot) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return fail("resolve");
    }
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    report_acquisition("commit_end",
                       "ok",
                       "published",
                       prepared.acquiredDefinitionHash,
                       prepared.characterSoid,
                       prepared.acquiredInstanceSoid,
                       prepared.inventoryIndex,
                       prepared.inventoryRow,
                       prepared.equipmentSlot,
                       prepared.afterCharacter.nextInventorySerial);
    return true;
}

/** Prepares one checked profile-stack increment or append for a Collections pull. */
bool prepare_profile_item_acquisition(std::uint16_t collectibleIndex,
                                      std::uint32_t definitionHash,
                                      PendingProfileItemAcquisition& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    build_data::collectibles::Definition collectible{};
    build_data::items::Definition item{};
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    if (definitionHash == authored_inventory::kNoDefinitionHash || !account::valid(account)
        || !valid_profile_inventory(account)
        || !build_data::find_collectible_definition(collectibleIndex, collectible)
        || collectible.itemDefinitionIndex
               == build_data::collectibles::kUnavailableItemDefinitionIndex
        || !build_data::find_item_definition_hash(definitionHash, item)
        || item.definitionHash != definitionHash
        || item.definitionIndex != collectible.itemDefinitionIndex
        || !build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex || detail.definitionHash != definitionHash
        || detail.bucketId != item.bucketId
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
        || detail.maxStackSize <= 0
        || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
        || bucket.arraySelector != inventory_buckets::ArraySelector::profile) {
        report_profile_acquisition("prepare",
                                   "fail",
                                   "definition_or_profile",
                                   definitionHash,
                                   account.primarySoid,
                                   0,
                                   0,
                                   0,
                                   account.profileItemCount,
                                   0,
                                   0,
                                   false);
        return false;
    }
    AccountState chargedAccount{};
    bool materialsChanged = false;
    if (!apply_collection_materials(account, collectible, chargedAccount, materialsChanged)) {
        report_profile_acquisition("prepare",
                                   "fail",
                                   "materials",
                                   definitionHash,
                                   account.primarySoid,
                                   0,
                                   detail.bucketId,
                                   0,
                                   account.profileItemCount,
                                   0,
                                   0,
                                   false);
        return false;
    }
    (void)materialsChanged;
    const bool actionSource =
        build_data::is_profile_action_source(item.definitionIndex, item.bucketId);

    std::size_t profileIndex = chargedAccount.profileItemCount;
    std::int32_t previousQuantity = 0;
    std::int32_t previousMutationSerial = 0;
    std::int32_t greatestMutationSerial = 0;
    bool appended = true;
    for (std::size_t index = 0; index < chargedAccount.profileItemCount; ++index) {
        greatestMutationSerial =
            (std::max)(greatestMutationSerial, chargedAccount.profileItems[index].mutationSerial);
    }
    for (std::size_t index = 0; index < chargedAccount.profileItemCount; ++index) {
        const authored_inventory::ProfileItem& existing = chargedAccount.profileItems[index];
        if (existing.definitionHash != definitionHash) {
            continue;
        }
        if (existing.quantity > detail.maxStackSize) {
            report_profile_acquisition("prepare",
                                       "fail",
                                       "existing_quantity",
                                       definitionHash,
                                       account.primarySoid,
                                       existing.instanceSoid,
                                       detail.bucketId,
                                       index,
                                       chargedAccount.profileItemCount,
                                       existing.quantity,
                                       existing.quantity,
                                       false);
            return false;
        }
        if (appended && existing.quantity < detail.maxStackSize) {
            profileIndex = index;
            previousQuantity = existing.quantity;
            previousMutationSerial = existing.mutationSerial;
            appended = false;
        }
    }
    if (greatestMutationSerial == (std::numeric_limits<std::int32_t>::max)()
        || (appended && chargedAccount.profileItemCount >= chargedAccount.profileItems.size())) {
        report_profile_acquisition(
            "prepare",
            "fail",
            "state_capacity",
            definitionHash,
            account.primarySoid,
            appended ? 0 : chargedAccount.profileItems[profileIndex].instanceSoid,
            detail.bucketId,
            profileIndex,
            chargedAccount.profileItemCount,
            0,
            0,
            true);
        return false;
    }

    std::uint64_t acquiredInstanceSoid =
        appended ? 0 : chargedAccount.profileItems[profileIndex].instanceSoid;
    if (actionSource != (acquiredInstanceSoid != 0) && !appended) {
        report_profile_acquisition("prepare",
                                   "fail",
                                   "identity_policy",
                                   definitionHash,
                                   account.primarySoid,
                                   acquiredInstanceSoid,
                                   detail.bucketId,
                                   profileIndex,
                                   chargedAccount.profileItemCount,
                                   previousQuantity,
                                   previousQuantity,
                                   false);
        return false;
    }
    if (appended && actionSource
        && !next_profile_item_instance_soid(chargedAccount, acquiredInstanceSoid)) {
        report_profile_acquisition("prepare",
                                   "fail",
                                   "identity_capacity",
                                   definitionHash,
                                   account.primarySoid,
                                   acquiredInstanceSoid,
                                   detail.bucketId,
                                   profileIndex,
                                   chargedAccount.profileItemCount,
                                   0,
                                   0,
                                   true);
        return false;
    }

    AccountState after = chargedAccount;
    const std::int32_t acquiredMutationSerial = greatestMutationSerial + 1;
    if (appended) {
        after.profileItems[profileIndex] = {
            acquiredInstanceSoid, definitionHash, 1, acquiredMutationSerial};
        ++after.profileItemCount;
    } else {
        ++after.profileItems[profileIndex].quantity;
        after.profileItems[profileIndex].mutationSerial = acquiredMutationSerial;
    }
    const std::int32_t acquiredQuantity = after.profileItems[profileIndex].quantity;
    if (after.profileItems[profileIndex].instanceSoid != acquiredInstanceSoid
        || acquiredQuantity <= previousQuantity || acquiredQuantity > detail.maxStackSize
        || !account::valid(after) || !valid_profile_inventory(after)) {
        report_profile_acquisition("prepare",
                                   "fail",
                                   "bucket_full_or_quantity",
                                   definitionHash,
                                   account.primarySoid,
                                   acquiredInstanceSoid,
                                   detail.bucketId,
                                   profileIndex,
                                   after.profileItemCount,
                                   previousQuantity,
                                   acquiredQuantity,
                                   appended);
        return false;
    }

    mutation.beforeItems = account.profileItems;
    mutation.afterItems = after.profileItems;
    mutation.accountSoid = account.primarySoid;
    mutation.acquiredInstanceSoid = acquiredInstanceSoid;
    mutation.acquiredDefinitionHash = definitionHash;
    mutation.materialRequirementSetHash = collectible.materialRequirementSetHash;
    mutation.expectedItemCount = account.profileItemCount;
    mutation.afterItemCount = after.profileItemCount;
    mutation.profileIndex = profileIndex;
    mutation.previousQuantity = previousQuantity;
    mutation.acquiredQuantity = acquiredQuantity;
    mutation.previousMutationSerial = previousMutationSerial;
    mutation.acquiredMutationSerial = acquiredMutationSerial;
    mutation.collectibleIndex = collectibleIndex;
    mutation.bucketId = detail.bucketId;
    mutation.materialRequirementCount = collectible.materialRequirementCount;
    mutation.actionSource = actionSource;
    mutation.appended = appended;
    mutation.prepared = true;
    if (!valid_profile_mutation_shape(mutation)) {
        mutation = {};
        report_profile_acquisition("prepare",
                                   "fail",
                                   "mutation",
                                   definitionHash,
                                   account.primarySoid,
                                   acquiredInstanceSoid,
                                   detail.bucketId,
                                   profileIndex,
                                   after.profileItemCount,
                                   previousQuantity,
                                   acquiredQuantity,
                                   appended);
        return false;
    }
    report_profile_acquisition("prepare",
                               "ok",
                               "ready",
                               definitionHash,
                               account.primarySoid,
                               acquiredInstanceSoid,
                               detail.bucketId,
                               profileIndex,
                               after.profileItemCount,
                               previousQuantity,
                               acquiredQuantity,
                               appended);
    return true;
}

/** Produces the exact account after-image while the captured profile view is still current. */
bool preview_profile_item_acquisition(const PendingProfileItemAcquisition& mutation,
                                      AccountState& after) noexcept {
    after = {};
    const AccountState current = account_snapshot();
    const bool ready = materialize_profile_acquisition(current, mutation, after);
    report_profile_acquisition("preview",
                               ready ? "ok" : "fail",
                               ready ? "ready" : "stale_or_invalid",
                               mutation.acquiredDefinitionHash,
                               mutation.accountSoid,
                               mutation.acquiredInstanceSoid,
                               mutation.bucketId,
                               mutation.profileIndex,
                               mutation.afterItemCount,
                               mutation.previousQuantity,
                               mutation.acquiredQuantity,
                               mutation.appended);
    return ready;
}

/** Commits one profile-stack after-image only while its exact prepare-time view remains current. */
bool commit_profile_item_acquisition(PendingProfileItemAcquisition& mutation) noexcept {
    const PendingProfileItemAcquisition prepared = mutation;
    mutation = {};
    if (!valid_profile_mutation_shape(prepared)) {
        report_profile_acquisition("commit",
                                   "fail",
                                   "mutation",
                                   prepared.acquiredDefinitionHash,
                                   prepared.accountSoid,
                                   prepared.acquiredInstanceSoid,
                                   prepared.bucketId,
                                   prepared.profileIndex,
                                   prepared.afterItemCount,
                                   prepared.previousQuantity,
                                   prepared.acquiredQuantity,
                                   prepared.appended);
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate{};
    const bool ready =
        materialize_profile_acquisition(runtime::storage::g_state.account, prepared, candidate);
    if (ready) {
        runtime::storage::g_state.account = candidate;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    report_profile_acquisition("commit",
                               ready ? "ok" : "fail",
                               ready ? "published" : "stale_or_invalid",
                               prepared.acquiredDefinitionHash,
                               prepared.accountSoid,
                               prepared.acquiredInstanceSoid,
                               prepared.bucketId,
                               prepared.profileIndex,
                               prepared.afterItemCount,
                               prepared.previousQuantity,
                               prepared.acquiredQuantity,
                               prepared.appended);
    return ready;
}

namespace {

/**
 * Default plug hashes the wheel seeds when granting or repairing the collection item's sockets.
 * All four are universal Common emotes from Bright Engrams, with no class or race restriction:
 * "Yes" (3184938442), "Nope" (48790291), "Casual Sit" (383973261), "Cheer" (2834933816).
 * Lane order follows the client's own wheel layout, confirmed empirically in-game:
 * lane 0 = top, lane 1 = bottom, lane 2 = left, lane 3 = right.
 */
constexpr std::uint32_t kYesEmoteDefinitionHash = 3184938442U;
constexpr std::uint32_t kNopeEmoteDefinitionHash = 48790291U;
constexpr std::uint32_t kCasualSitEmoteDefinitionHash = 383973261U;
constexpr std::uint32_t kCheerEmoteDefinitionHash = 2834933816U;

constexpr std::array<std::uint32_t, authored_inventory::kEmoteCollectionSocketLaneCount>
    kEmoteCollectionDefaultPlugHashes{
        kCheerEmoteDefinitionHash,     // lane 0 -- top
        kCasualSitEmoteDefinitionHash, // lane 1 -- bottom
        kYesEmoteDefinitionHash,       // lane 2 -- left
        kNopeEmoteDefinitionHash,      // lane 3 -- right
    };

/**
 * Resolves and cross-checks the "Emotes" collection item's own configured content. The detail row
 * is only read to validate the definition, so it stays local rather than reaching the caller.
 * @param definition Receives the matching native item-definition row.
 * @return True only when both rows agree with each other, carry no native equipment slot (the one
 *         trait that singles this item out among every character-scoped item), and declare exactly
 *         the expected 4 ordinary socket lanes.
 */
[[nodiscard]] bool
resolve_emote_collection_definition(build_data::items::Definition& definition) noexcept {
    item_details::Definition detail{};
    return build_data::find_item_definition_hash(authored_inventory::kEmoteCollectionDefinitionHash,
                                                 definition)
           && definition.definitionHash == authored_inventory::kEmoteCollectionDefinitionHash
           && build_data::find_configured_item_detail(definition.definitionIndex, detail)
           && detail.definitionIndex == definition.definitionIndex
           && detail.definitionHash == authored_inventory::kEmoteCollectionDefinitionHash
           && detail.bucketId == definition.bucketId && !detail.equipmentSlot.has_value()
           && detail.ordinarySocketState == item_details::OrdinarySocketState::present
           && detail.ordinarySocketCount == authored_inventory::kEmoteCollectionSocketLaneCount;
}

/**
 * Checks that every one of the collection item's real plug pool candidates is actually installed
 * and allowed in its intended lane, so a granted item can never carry a plug the client rejects.
 */
[[nodiscard]] bool default_plugs_valid(std::uint16_t collectionDefinitionIndex) noexcept {
    for (std::size_t lane = 0; lane < kEmoteCollectionDefaultPlugHashes.size(); ++lane) {
        build_data::items::Definition plugDefinition{};
        if (!build_data::find_item_definition_hash(kEmoteCollectionDefaultPlugHashes[lane],
                                                   plugDefinition)
            || !build_data::is_socket_plug_allowed(collectionDefinitionIndex,
                                                   static_cast<std::uint8_t>(lane),
                                                   plugDefinition.definitionIndex)) {
            return false;
        }
    }
    return true;
}

/**
 * Checks an already-equipped collection item's own socket state, so a corrupted or stale set of
 * plugs is repaired instead of trusted just because the definition hash already matches.
 */
[[nodiscard]] bool socket_state_sound(const authored_inventory::Item& item,
                                      std::uint16_t collectionDefinitionIndex) noexcept {
    if (item.sockets.policy != authored_inventory::SocketPolicy::authored
        || item.sockets.plugCount != authored_inventory::kEmoteCollectionSocketLaneCount) {
        return false;
    }
    for (std::size_t lane = 0; lane < authored_inventory::kEmoteCollectionSocketLaneCount; ++lane) {
        const std::optional<std::uint32_t>& plugHash = item.sockets.plugs[lane];
        build_data::items::Definition plugDefinition{};
        if (!plugHash.has_value()
            || !build_data::find_item_definition_hash(*plugHash, plugDefinition)
            || !build_data::is_socket_plug_allowed(collectionDefinitionIndex,
                                                   static_cast<std::uint8_t>(lane),
                                                   plugDefinition.definitionIndex)) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * Equips each character with the "Emotes" collection item in the real emote slot, in place of an
 * individual emote. Unlike every other character-scoped item, its real content carries no native
 * equipment-slot mapping at all, so the resolvers this depends on (loadout resolution, light,
 * appearance refresh) fall back to authored_inventory::kEmoteCollectionNativeEquipmentSlot for it
 * specifically, gated to its exact definition hash (state::account::inventory::
 * resolve_native_equipment_slot). Its 4 ordinary sockets carry no native default plug, so 4 hashes
 * from its real reusable plug pool seed a default wheel; the client's own generic socket-plug
 * request (opcode 1901) lets the player reassign them afterward, the same mechanism it already uses
 * for weapon mods and shaders.
 */
EmoteCollectionOutcome ensure_character_emote_collection() noexcept {
    constexpr std::size_t kEmoteCollectionSlot =
        static_cast<std::size_t>(authored_inventory::EquipmentSlot::emote);

    // The domains every check below reads have to be published first. Until they are, nothing can
    // be concluded about the installed content, so this is a retry rather than a verdict.
    if (!build_data::item_definitions_ready() || !build_data::configured_item_details_ready()
        || !build_data::socket_plug_rules_ready()) {
        return EmoteCollectionOutcome::notReady;
    }
    // With those published, an item that still does not resolve this way is a build that cannot
    // carry the wheel at all. Retrying that within this process would never change the answer.
    build_data::items::Definition collectionDefinition{};
    if (!resolve_emote_collection_definition(collectionDefinition)
        || !default_plugs_valid(collectionDefinition.definitionIndex)) {
        return EmoteCollectionOutcome::unsupported;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return EmoteCollectionOutcome::notReady;
    }
    bool changed = false;
    bool failed = false;
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount && !failed;
         ++characterIndex) {
        CharacterState& character = candidate.characters[characterIndex];
        auto& collectionSlot = character.equipment.slots[kEmoteCollectionSlot];
        const bool present =
            collectionSlot.has_value()
            && collectionSlot->definitionHash == authored_inventory::kEmoteCollectionDefinitionHash;
        if (present && socket_state_sound(*collectionSlot, collectionDefinition.definitionIndex)) {
            continue;
        }
        if (character.nextInventorySerial
            >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
            failed = true;
            break;
        }
        // A repair owns only the definition, the sockets and the serial. Everything else the item
        // already carries, the accumulated item-state flags above all, belongs to the player and
        // survives. The account was checked whole on entry, so a present item's remaining scalars
        // are already known good and need no normalizing here.
        authored_inventory::Item granted = present ? *collectionSlot : authored_inventory::Item{};
        if (!present) {
            std::uint64_t instanceSoid = 0;
            if (!next_item_instance_soid(candidate, instanceSoid)) {
                failed = true;
                break;
            }
            granted.instanceSoid = instanceSoid;
            granted.level = 0;
            granted.quantity = 1;
        }
        granted.definitionHash = authored_inventory::kEmoteCollectionDefinitionHash;
        granted.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
        // Replaced whole rather than edited: the lanes past the used prefix have to be empty for
        // the socket block to validate, whatever the malformed copy left behind.
        granted.sockets = authored_inventory::Sockets{};
        granted.sockets.policy = authored_inventory::SocketPolicy::authored;
        granted.sockets.plugCount = kEmoteCollectionDefaultPlugHashes.size();
        for (std::size_t lane = 0; lane < kEmoteCollectionDefaultPlugHashes.size(); ++lane) {
            granted.sockets.plugs[lane] = kEmoteCollectionDefaultPlugHashes[lane];
        }
        collectionSlot = granted;
        changed = true;
    }
    if (failed) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return EmoteCollectionOutcome::failed;
    }
    if (!changed) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return EmoteCollectionOutcome::ready;
    }
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return EmoteCollectionOutcome::failed;
    }
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return EmoteCollectionOutcome::ready;
}

} // namespace sunrise::state
