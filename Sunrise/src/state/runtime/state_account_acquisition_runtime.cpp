#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

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
namespace inventory_buckets = build_data::inventory::buckets;
namespace family4_loadout = middleware::datagen::family4::loadout;

namespace runtime::detail {

using Quest = build_data::items::QuestInitialization;

/**
 * The plan must already be valid and nonempty before selecting a save bank.
 * @param quest First-step plan with account or character scope.
 * @return The persistent value bank for that scope.
 */
[[nodiscard]] investment::store::Bank quest_bank(const Quest& quest) noexcept {
    return quest.scope == Quest::Scope::account ? investment::store::Bank::objectiveValues
                                                : investment::store::Bank::characterObjectValues;
}

/**
 * Hold investment::store::g_mutex and validate the selected character before this check.
 * @param mutation Prepared acquisition with the prior saved quest value.
 * @return True only while item metadata and the saved quest value still match.
 */
[[nodiscard]] bool quest_current(const PendingItemAcquisition& mutation) noexcept {
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_hash(mutation.acquiredDefinitionHash, definition)
        || definition.questInitialization != mutation.questInitialization
        || !build_data::items::valid(mutation.questInitialization)) {
        return false;
    }
    if (mutation.questInitialization.scope == Quest::Scope::none) {
        return mutation.previousQuestValue == build_data::items::kUnsetQuestValue;
    }
    std::int32_t current = 0;
    return investment::store::read_unlock(
               quest_bank(mutation.questInitialization), mutation.questInitialization.row, current)
           && current == mutation.previousQuestValue;
}

/** @return The selected character's index, or the character count when none is selected. */
[[nodiscard]] std::size_t selected_character_index(const AccountState& account) noexcept {
    const std::size_t count = (std::min)(account.characterCount, account.characters.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (account.characters[index].selected) {
            return index;
        }
    }
    return account.characters.size();
}

/**
 * Hold investment::store::g_mutex while capturing inventory and quest state together.
 * @param account State before any acquisition charge.
 * @param chargedAccount State after the prepared material charge.
 * @param definitionHash Item definition to grant.
 * @param profileChanged Whether the charge changed profile inventory.
 * @param source Grant identity and material requirements for commit checks.
 * @param mutation Receives a pending grant; use only on success.
 * @return False when the item, inventory, mapping, or saved quest state is invalid.
 */
[[nodiscard]] bool finalize_item_acquisition(const AccountState& account,
                                             const AccountState& chargedAccount,
                                             std::uint32_t definitionHash,
                                             bool profileChanged,
                                             const GrantSource& source,
                                             PendingItemAcquisition& mutation) noexcept {
    const std::size_t characterIndex = selected_character_index(account);
    if (characterIndex >= account.characterCount) {
        return false;
    }

    const CharacterState& before = account.characters[characterIndex];
    if (before.inventory.count >= before.inventory.values.size()
        || before.nextInventorySerial
               >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }

    std::uint64_t instanceSoid = 0;
    if (!next_item_instance_soid(account, instanceSoid)) {
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
        || !find_unequipped_row(resolved, instanceSoid, inventoryRow, equipmentSlot)) {
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
    mutation.materialRequirementSetHash = source.materialRequirementSetHash;
    mutation.characterIndex = characterIndex;
    mutation.expectedInventoryCount = before.inventory.count;
    mutation.expectedProfileItemCount = account.profileItemCount;
    mutation.afterProfileItemCount = chargedAccount.profileItemCount;
    mutation.inventoryIndex = inventoryIndex;
    mutation.collectibleIndex = source.collectibleIndex;
    mutation.inventoryRow = inventoryRow;
    mutation.equipmentSlot = equipmentSlot;
    mutation.materialRequirementCount = source.materialRequirementCount;
    mutation.profileChanged = profileChanged;
    mutation.directGrant = source.direct;
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_hash(definitionHash, definition)
        || !build_data::items::valid(definition.questInitialization)) {
        return false;
    }
    mutation.questInitialization = definition.questInitialization;
    if (mutation.questInitialization.scope != Quest::Scope::none
        && !investment::store::read_unlock(quest_bank(mutation.questInitialization),
                                           mutation.questInitialization.row,
                                           mutation.previousQuestValue)) {
        return false;
    }
    mutation.prepared = true;
    return true;
}

} // namespace runtime::detail

/**
 * Inventory and quest state must come from the same locked save view.
 * @param collectibleIndex Collections row, or kNoCollectibleIndex for an item-only grant.
 * @param definitionHash Item definition to grant.
 * @param mutation Receives a pending grant; prepared is set only on success.
 * @return False when identity, costs, capacity, or saved state prevent the grant.
 */
bool prepare_item_acquisition(std::uint16_t collectibleIndex,
                              std::uint32_t definitionHash,
                              PendingItemAcquisition& mutation) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    mutation = {};
    const AccountState account = account_snapshot();
    build_data::collectibles::Definition collectible{};
    build_data::items::Definition grantedDefinition{};
    // A vendor purchase names an item, not a collectible, so the collectible steps are skipped
    // rather than faked. The item is still validated, just by its own hash.
    const bool hasCollectible = collectibleIndex != build_data::collectibles::kNoCollectibleIndex;
    if (definitionHash == authored_inventory::kNoDefinitionHash || !account::valid(account)
        || !valid_profile_inventory(account)) {
        return false;
    }
    if (hasCollectible) {
        if (!build_data::find_collectible_definition(collectibleIndex, collectible)
            || collectible.itemDefinitionIndex
                   == build_data::collectibles::kUnavailableItemDefinitionIndex
            || !build_data::find_item_definition_index(collectible.itemDefinitionIndex,
                                                       grantedDefinition)
            || grantedDefinition.definitionHash != definitionHash) {
            return false;
        }
    } else if (!build_data::find_item_definition_hash(definitionHash, grantedDefinition)
               || grantedDefinition.definitionHash != definitionHash) {
        return false;
    }

    AccountState chargedAccount = account;
    bool profileChanged = false;
    // Nothing is charged without a collectible: the cost lives on the collectible's material
    // requirements, and a sale row's own cost fields are still role-open.
    if (hasCollectible
        && !apply_collection_materials(account, collectible, chargedAccount, profileChanged)) {
        return false;
    }

    return finalize_item_acquisition(
        account,
        chargedAccount,
        definitionHash,
        profileChanged,
        {.materialRequirementSetHash = collectible.materialRequirementSetHash,
         .collectibleIndex = collectibleIndex,
         .materialRequirementCount = collectible.materialRequirementCount},
        mutation);
}

/**
 * Direct grants share quest-state checks but do not charge Collections materials.
 * @param itemDefinitionIndex Item-table row to grant to the selected character.
 * @param mutation Receives a pending grant; prepared is set only on success.
 * @return False when the item, inventory, mapping, or saved quest state is invalid.
 */
bool prepare_item_acquisition_for_item(std::uint16_t itemDefinitionIndex,
                                       PendingItemAcquisition& mutation) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    mutation = {};
    const AccountState account = account_snapshot();
    build_data::items::Definition grantedDefinition{};
    if (!account::valid(account) || !valid_profile_inventory(account)
        || !build_data::find_item_definition_index(itemDefinitionIndex, grantedDefinition)
        || grantedDefinition.definitionHash == authored_inventory::kNoDefinitionHash) {
        return false;
    }

    return finalize_item_acquisition(
        account, account, grantedDefinition.definitionHash, false, {.direct = true}, mutation);
}

/** Prepares a fixed Season wrapper expansion without exposing a partial package. */
bool prepare_direct_item_bundle(std::uint32_t sourceDefinitionHash,
                                std::span<const std::uint16_t> itemDefinitionIndices,
                                PendingDirectItemBundle& mutation) noexcept {
    mutation = {};
    build_data::season_pass::Package package{};
    if (!build_data::find_season_pass_package(sourceDefinitionHash, package)
        || itemDefinitionIndices.size() != package.itemCount) {
        return false;
    }

    std::array<std::uint32_t, build_data::season_pass::kPackageItemCapacity> hashes{};
    for (std::size_t index = 0; index < itemDefinitionIndices.size(); ++index) {
        const std::uint16_t definitionIndex = itemDefinitionIndices[index];
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (!build_data::find_item_definition_index(definitionIndex, definition)
            || definition.definitionHash != package.items[index]
            || !build_data::find_configured_item_detail(definitionIndex, detail)
            || detail.definitionIndex != definition.definitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::instanced
            || !detail.equipmentSlot.has_value()
            || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::character) {
            return false;
        }
        hashes[index] = definition.definitionHash;
    }

    const AccountState account = account_snapshot();
    const std::size_t characterIndex = selected_character_index(account);
    if (!account::valid(account) || characterIndex >= account.characterCount) {
        return false;
    }
    const CharacterState& before = account.characters[characterIndex];
    if (before.nextInventorySerial
            > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
        || itemDefinitionIndices.size() > before.inventory.values.size() - before.inventory.count
        || itemDefinitionIndices.size()
               > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())
                     - before.nextInventorySerial) {
        return false;
    }

    std::uint64_t firstSoid = 0;
    if (!next_item_instance_soid(account, firstSoid)
        || itemDefinitionIndices.size() - 1U
               > (std::numeric_limits<std::uint64_t>::max)() - firstSoid) {
        return false;
    }

    CharacterState after = before;
    const std::int32_t level = acquisition_level(before);
    for (std::size_t index = 0; index < itemDefinitionIndices.size(); ++index) {
        authored_inventory::Item granted{};
        granted.instanceSoid = firstSoid + index;
        granted.definitionHash = hashes[index];
        granted.level = level;
        granted.quantity = 1;
        granted.mutationSerial = static_cast<std::int32_t>(after.nextInventorySerial++);
        after.inventory.values[after.inventory.count++] = granted;
    }

    AccountState candidate = account;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout resolved{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, resolved)) {
        return false;
    }
    for (std::size_t index = 0; index < itemDefinitionIndices.size(); ++index) {
        std::uint16_t row = 0;
        std::uint8_t slot = 0;
        if (!find_unequipped_row(resolved, firstSoid + index, row, slot)) {
            return false;
        }
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.firstInstanceSoid = firstSoid;
    mutation.sourceDefinitionHash = sourceDefinitionHash;
    mutation.characterIndex = characterIndex;
    mutation.expectedInventoryCount = before.inventory.count;
    mutation.itemCount = itemDefinitionIndices.size();
    mutation.prepared = true;
    return true;
}

/**
 * Takes the selected character's next inventory mutation serial under the State lock.
 * @param mutationSerial Receives the reserved serial; zero when nothing was reserved.
 * @return False when no character is selected or its serial space is exhausted.
 */
bool reserve_selected_character_inventory_serial(std::int32_t& mutationSerial) noexcept {
    mutationSerial = 0;
    investment::store::g_mutex.lock();
    AccountState account = investment::store::account();
    const std::size_t characterIndex = selected_character_index(account);
    bool ready = account::valid(account) && characterIndex < account.characterCount
                 && account.characters[characterIndex].nextInventorySerial
                        < static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    if (ready) {
        mutationSerial =
            static_cast<std::int32_t>(account.characters[characterIndex].nextInventorySerial++);
    }
    if (ready) {
        ready = investment::store::write_account(account);
    }
    if (!ready) {
        mutationSerial = 0;
    }
    investment::store::g_mutex.unlock();
    return ready;
}

namespace runtime::detail {

/**
 * Checks that a prepared insertion still agrees with its Collections row or direct grant.
 * @return False when the mutation's shape, its cost fields, or its item no longer hold.
 */
[[nodiscard]] static bool
valid_item_acquisition_source(const PendingItemAcquisition& mutation) noexcept {
    if (!mutation.prepared || mutation.characterSoid == 0 || mutation.acquiredInstanceSoid == 0
        || mutation.accountSoid == 0
        || mutation.acquiredDefinitionHash == authored_inventory::kNoDefinitionHash
        || mutation.characterIndex >= kCharacterCapacity
        || mutation.expectedInventoryCount >= authored_inventory::kCharacterItemCapacity
        || mutation.inventoryIndex != mutation.expectedInventoryCount
        || mutation.afterCharacter.inventory.count != mutation.expectedInventoryCount + 1U
        || mutation.inventoryIndex >= mutation.afterCharacter.inventory.count
        || mutation.afterCharacter.inventory.values[mutation.inventoryIndex].instanceSoid
               != mutation.acquiredInstanceSoid
        || mutation.afterCharacter.inventory.values[mutation.inventoryIndex].definitionHash
               != mutation.acquiredDefinitionHash
        || mutation.expectedProfileItemCount > authored_inventory::kProfileItemCapacity
        || mutation.afterProfileItemCount > authored_inventory::kProfileItemCapacity) {
        return false;
    }
    if (mutation.directGrant) {
        build_data::items::Definition definition{};
        return mutation.collectibleIndex == 0 && mutation.materialRequirementSetHash == 0
               && mutation.materialRequirementCount == 0
               && same_profile_views(mutation.beforeProfileItems,
                                     mutation.expectedProfileItemCount,
                                     mutation.afterProfileItems,
                                     mutation.afterProfileItemCount)
               && build_data::find_item_definition_hash(mutation.acquiredDefinitionHash,
                                                        definition);
    }

    if (mutation.collectibleIndex == build_data::collectibles::kNoCollectibleIndex) {
        // The guard is that prepare and commit agree. Without a collectible they agree on there
        // being none, which means both cost fields must still be clear.
        build_data::items::Definition definition{};
        return mutation.materialRequirementSetHash == 0 && mutation.materialRequirementCount == 0
               && build_data::find_item_definition_hash(mutation.acquiredDefinitionHash,
                                                        definition);
    }

    build_data::collectibles::Definition collectible{};
    build_data::items::Definition definition{};
    return build_data::find_collectible_definition(mutation.collectibleIndex, collectible)
           && collectible.itemDefinitionIndex
                  != build_data::collectibles::kUnavailableItemDefinitionIndex
           && collectible.materialRequirementSetHash == mutation.materialRequirementSetHash
           && collectible.materialRequirementCount == mutation.materialRequirementCount
           && build_data::find_item_definition_index(collectible.itemDefinitionIndex, definition)
           && definition.definitionHash == mutation.acquiredDefinitionHash;
}

/**
 * Hold investment::store::g_mutex; the selected character and saved state must still match.
 * @param current Current account from the locked save view.
 * @param mutation Prepared inventory insertion and prior quest state.
 * @param after Receives the candidate account; use only on success.
 * @return False for stale state or an invalid resulting inventory.
 */
[[nodiscard]] bool materialize_item_acquisition(const AccountState& current,
                                                const PendingItemAcquisition& mutation,
                                                AccountState& after) noexcept {
    std::uint64_t nextSoid = 0;
    if (!valid_item_acquisition_source(mutation)
        || mutation.characterIndex >= current.characterCount
        || !current.characters[mutation.characterIndex].selected
        || current.characters[mutation.characterIndex].soid != mutation.characterSoid
        || !quest_current(mutation) || current.primarySoid != mutation.accountSoid
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.expectedProfileItemCount)
        || !next_item_instance_soid(current, nextSoid)
        || nextSoid != mutation.acquiredInstanceSoid) {
        return false;
    }

    after = current;
    after.profileItems = mutation.afterProfileItems;
    after.profileItemCount = mutation.afterProfileItemCount;
    after.characters[mutation.characterIndex] = mutation.afterCharacter;
    family4_loadout::ResolvedLoadout resolved{};
    std::uint16_t row = 0;
    std::uint8_t slot = 0;
    return account::valid(after) && valid_profile_inventory(after)
           && family4_loadout::resolve(after, mutation.characterIndex, resolved)
           && find_unequipped_row(resolved, mutation.acquiredInstanceSoid, row, slot)
           && row == mutation.inventoryRow && slot == mutation.equipmentSlot;
}

/** Rebuilds one package from installed policy and rejects any altered after-image. */
[[nodiscard]] bool materialize_direct_item_bundle(const AccountState& current,
                                                  const PendingDirectItemBundle& mutation,
                                                  AccountState& after) noexcept {
    build_data::season_pass::Package package{};
    std::uint64_t firstSoid = 0;
    if (!mutation.prepared
        || !build_data::find_season_pass_package(mutation.sourceDefinitionHash, package)
        || mutation.itemCount != package.itemCount || mutation.accountSoid == 0
        || mutation.characterSoid == 0 || mutation.firstInstanceSoid == 0
        || mutation.characterIndex >= current.characterCount
        || mutation.expectedInventoryCount >= authored_inventory::kCharacterItemCapacity
        || current.primarySoid != mutation.accountSoid
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !current.characters[mutation.characterIndex].selected
        || current.characters[mutation.characterIndex].soid != mutation.characterSoid
        || mutation.beforeCharacter.inventory.count != mutation.expectedInventoryCount
        || mutation.itemCount
               > mutation.beforeCharacter.inventory.values.size() - mutation.expectedInventoryCount
        || mutation.beforeCharacter.nextInventorySerial
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
        || mutation.itemCount > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())
                                    - mutation.beforeCharacter.nextInventorySerial
        || !next_item_instance_soid(current, firstSoid) || firstSoid != mutation.firstInstanceSoid
        || mutation.itemCount - 1U > (std::numeric_limits<std::uint64_t>::max)() - firstSoid) {
        return false;
    }

    CharacterState canonical = mutation.beforeCharacter;
    const std::int32_t level = acquisition_level(canonical);
    for (std::size_t index = 0; index < mutation.itemCount; ++index) {
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (!build_data::find_item_definition_hash(package.items[index], definition)
            || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.definitionHash != definition.definitionHash
            || detail.definitionIndex != definition.definitionIndex
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::instanced
            || !detail.equipmentSlot.has_value()
            || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::character) {
            return false;
        }
        authored_inventory::Item granted{};
        granted.instanceSoid = firstSoid + index;
        granted.definitionHash = package.items[index];
        granted.level = level;
        granted.quantity = 1;
        granted.mutationSerial = static_cast<std::int32_t>(canonical.nextInventorySerial++);
        canonical.inventory.values[canonical.inventory.count++] = granted;
    }
    if (!same_character(canonical, mutation.afterCharacter)) {
        return false;
    }

    after = current;
    after.characters[mutation.characterIndex] = canonical;
    family4_loadout::ResolvedLoadout resolved{};
    if (!account::valid(after)
        || !family4_loadout::resolve(after, mutation.characterIndex, resolved)) {
        return false;
    }
    for (std::size_t index = 0; index < mutation.itemCount; ++index) {
        std::uint16_t row = 0;
        std::uint8_t slot = 0;
        if (!find_unequipped_row(resolved, firstSoid + index, row, slot)) {
            return false;
        }
    }
    return true;
}

} // namespace runtime::detail

/**
 * Preview inventory and quest values together without changing the save.
 * @param mutation Prepared acquisition checked against current saved state.
 * @param after Receives the candidate account; use only on success.
 * @param afterUnlocks Receives matching account and selected-character unlocks on success.
 * @return False when the acquisition is stale or its saved unlocks cannot be read.
 */
bool preview_item_acquisition(const PendingItemAcquisition& mutation,
                              AccountState& after,
                              unlocks::Table& afterUnlocks) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    after = {};
    afterUnlocks = {};
    if (!materialize_item_acquisition(account_snapshot(), mutation, after)
        || !investment::store::read_unlocks(afterUnlocks,
                                            static_cast<int>(mutation.characterIndex))) {
        return false;
    }
    const auto& quest = mutation.questInitialization;
    const auto value = build_data::items::initialized_value(quest, mutation.previousQuestValue);
    if (quest.scope == Quest::Scope::account) {
        afterUnlocks.objectiveValues[quest.row] = value;
    } else if (quest.scope == Quest::Scope::character) {
        afterUnlocks.characterObjectValues[quest.row] = value;
    }
    return true;
}

/** Produces the full account after-image while a prepared package remains current. */
bool preview_direct_item_bundle(const PendingDirectItemBundle& mutation,
                                AccountState& after) noexcept {
    after = {};
    return materialize_direct_item_bundle(account_snapshot(), mutation, after);
}

/**
 * Inventory and first-step state share one transaction; failure rolls both back.
 * @param mutation Prepared grant consumed on either success or failure.
 * @return True when both writes commit against the unchanged prepared state.
 */
bool commit_item_acquisition(PendingItemAcquisition& mutation) noexcept {
    const PendingItemAcquisition& prepared = mutation;
    const PendingConsumption consume{mutation};
    investment::store::Transaction transaction;
    AccountState candidate{};
    if (!transaction.ready()
        || !materialize_item_acquisition(investment::store::account(), prepared, candidate)
        || !investment::store::write_account(candidate)) {
        return false;
    }
    const auto& quest = prepared.questInitialization;
    if (quest.scope != Quest::Scope::none
        && prepared.previousQuestValue == build_data::items::kUnsetQuestValue
        && !investment::store::write_unlock(quest_bank(quest), quest.row, quest.value)) {
        return false;
    }
    return transaction.commit();
}

namespace runtime::detail {

/**
 * Resolves one stackable profile-bucket item with its configured detail row.
 * @param item Receives the item definition.
 * @param detail Receives the configured detail row.
 * @return False when the two rows disagree or the item is not a stackable profile item.
 */
[[nodiscard]] static bool resolve_profile_item(std::uint16_t itemDefinitionIndex,
                                               build_data::items::Definition& item,
                                               item_details::Definition& detail) noexcept {
    inventory_buckets::Descriptor bucket{};
    return build_data::find_item_definition_index(itemDefinitionIndex, item)
           && item.definitionHash != authored_inventory::kNoDefinitionHash
           && build_data::find_configured_item_detail(itemDefinitionIndex, detail)
           && detail.definitionIndex == item.definitionIndex
           && detail.definitionHash == item.definitionHash && detail.bucketId == item.bucketId
           && detail.instancedDefinitionState == item_details::InstancedDefinitionState::stackable
           && detail.maxStackSize > 0
           && build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
           && bucket.arraySelector == inventory_buckets::ArraySelector::profile;
}

/** Stages the common profile-stack insertion path. */
[[nodiscard]] bool
finalize_profile_item_acquisition(const AccountState& account,
                                  const AccountState& chargedAccount,
                                  std::uint32_t definitionHash,
                                  const item_details::Definition& detail,
                                  bool actionSource,
                                  std::int32_t quantity,
                                  const GrantSource& source,
                                  PendingProfileItemAcquisition& mutation) noexcept {
    if (quantity <= 0 || quantity > detail.maxStackSize) {
        return false;
    }
    std::size_t profileIndex = chargedAccount.profileItemCount;
    std::int32_t previousQuantity = 0;
    std::int32_t previousMutationSerial = 0;
    std::int32_t greatestMutationSerial = 0;
    bool appended = true;
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        greatestMutationSerial =
            (std::max)(greatestMutationSerial, account.profileItems[index].mutationSerial);
    }
    for (std::size_t index = 0; index < chargedAccount.profileItemCount; ++index) {
        const authored_inventory::ProfileItem& existing = chargedAccount.profileItems[index];
        greatestMutationSerial = (std::max)(greatestMutationSerial, existing.mutationSerial);
        if (existing.definitionHash != definitionHash) {
            continue;
        }
        if (existing.quantity > detail.maxStackSize) {
            return false;
        }
        if (appended && existing.quantity <= detail.maxStackSize - quantity) {
            profileIndex = index;
            previousQuantity = existing.quantity;
            previousMutationSerial = existing.mutationSerial;
            appended = false;
        }
    }
    if (greatestMutationSerial == (std::numeric_limits<std::int32_t>::max)()
        || (appended && chargedAccount.profileItemCount >= chargedAccount.profileItems.size())
        || quantity > detail.maxStackSize - previousQuantity) {
        return false;
    }

    std::uint64_t acquiredInstanceSoid =
        appended ? 0 : chargedAccount.profileItems[profileIndex].instanceSoid;
    if (!appended && actionSource != (acquiredInstanceSoid != 0)) {
        return false;
    }
    if (appended && actionSource
        && !next_profile_item_instance_soid(chargedAccount, acquiredInstanceSoid)) {
        return false;
    }

    AccountState after = chargedAccount;
    const std::int32_t acquiredMutationSerial = greatestMutationSerial + 1;
    if (appended) {
        after.profileItems[profileIndex] = {
            acquiredInstanceSoid, definitionHash, quantity, acquiredMutationSerial};
        ++after.profileItemCount;
    } else {
        after.profileItems[profileIndex].quantity += quantity;
        after.profileItems[profileIndex].mutationSerial = acquiredMutationSerial;
    }
    const std::int32_t acquiredQuantity = after.profileItems[profileIndex].quantity;
    if (after.profileItems[profileIndex].instanceSoid != acquiredInstanceSoid
        || acquiredQuantity <= previousQuantity || acquiredQuantity > detail.maxStackSize
        || !account::valid(after) || !valid_profile_inventory(after)) {
        return false;
    }

    mutation.beforeItems = account.profileItems;
    mutation.afterItems = after.profileItems;
    mutation.accountSoid = account.primarySoid;
    mutation.acquiredInstanceSoid = acquiredInstanceSoid;
    mutation.acquiredDefinitionHash = definitionHash;
    mutation.materialRequirementSetHash = source.materialRequirementSetHash;
    mutation.expectedItemCount = account.profileItemCount;
    mutation.afterItemCount = after.profileItemCount;
    mutation.profileIndex = profileIndex;
    mutation.previousQuantity = previousQuantity;
    mutation.acquiredQuantity = acquiredQuantity;
    mutation.previousMutationSerial = previousMutationSerial;
    mutation.acquiredMutationSerial = acquiredMutationSerial;
    mutation.collectibleIndex = source.collectibleIndex;
    mutation.bucketId = detail.bucketId;
    mutation.materialRequirementCount = source.materialRequirementCount;
    mutation.actionSource = actionSource;
    mutation.appended = appended;
    mutation.directGrant = source.direct;
    mutation.prepared = true;
    if (valid_profile_mutation_shape(mutation)) {
        return true;
    }
    mutation = {};
    return false;
}

} // namespace runtime::detail

/** Prepares one checked profile-stack increment or append for a Collections pull. */
bool prepare_profile_item_acquisition(std::uint16_t collectibleIndex,
                                      std::uint32_t definitionHash,
                                      PendingProfileItemAcquisition& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    build_data::collectibles::Definition collectible{};
    build_data::items::Definition item{};
    item_details::Definition detail{};
    if (definitionHash == authored_inventory::kNoDefinitionHash || !account::valid(account)
        || !valid_profile_inventory(account)
        || !build_data::find_item_definition_hash(definitionHash, item)
        // The item resolves first, because the collectible cross-check reads it. With no
        // collectible the item's own hash is the whole check.
        || (collectibleIndex != build_data::collectibles::kNoCollectibleIndex
            && (!build_data::find_collectible_definition(collectibleIndex, collectible)
                || collectible.itemDefinitionIndex
                       == build_data::collectibles::kUnavailableItemDefinitionIndex
                || item.definitionIndex != collectible.itemDefinitionIndex))
        || !resolve_profile_item(item.definitionIndex, item, detail)
        || item.definitionHash != definitionHash) {
        return false;
    }
    AccountState chargedAccount = account;
    bool materialsChanged = false;
    // Nothing is charged without a collectible: the cost lives on the collectible's material
    // requirements, and a sale row's own cost fields are still role-open.
    if (collectibleIndex != build_data::collectibles::kNoCollectibleIndex
        && !apply_collection_materials(account, collectible, chargedAccount, materialsChanged)) {
        return false;
    }
    (void)materialsChanged;
    const bool actionSource =
        build_data::is_profile_action_source(item.definitionIndex, item.bucketId);

    return finalize_profile_item_acquisition(
        account,
        chargedAccount,
        definitionHash,
        detail,
        actionSource,
        1,
        {.materialRequirementSetHash = collectible.materialRequirementSetHash,
         .collectibleIndex = collectibleIndex,
         .materialRequirementCount = collectible.materialRequirementCount},
        mutation);
}

/** Prepares one direct profile-stack grant, with no Collections row or material charge. */
bool prepare_profile_item_acquisition_for_item(std::uint16_t itemDefinitionIndex,
                                               std::int32_t quantity,
                                               PendingProfileItemAcquisition& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    build_data::items::Definition item{};
    item_details::Definition detail{};
    if (quantity <= 0 || !account::valid(account) || !valid_profile_inventory(account)
        || !resolve_profile_item(itemDefinitionIndex, item, detail)) {
        return false;
    }
    const bool actionSource =
        build_data::is_profile_action_source(item.definitionIndex, item.bucketId);

    return finalize_profile_item_acquisition(account,
                                             account,
                                             item.definitionHash,
                                             detail,
                                             actionSource,
                                             quantity,
                                             {.direct = true},
                                             mutation);
}

/** Produces the exact account after-image while the captured profile view is still current. */
bool preview_profile_item_acquisition(const PendingProfileItemAcquisition& mutation,
                                      AccountState& after) noexcept {
    after = {};
    const AccountState current = account_snapshot();
    return materialize_profile_acquisition(current, mutation, after);
}

/** Commits one profile-stack after-image only while its exact prepare-time view remains current. */
bool commit_profile_item_acquisition(PendingProfileItemAcquisition& mutation) noexcept {
    const PendingProfileItemAcquisition& prepared = mutation;
    const PendingConsumption consume{mutation};
    if (!valid_profile_mutation_shape(prepared)) {
        return false;
    }

    investment::store::g_mutex.lock();
    AccountState candidate{};
    const bool ready =
        materialize_profile_acquisition(investment::store::account(), prepared, candidate);
    if (ready) {
        if (!investment::store::write_account(candidate)) {
            investment::store::g_mutex.unlock();
            return false;
        }
    }
    investment::store::g_mutex.unlock();
    return ready;
}

} // namespace sunrise::state
