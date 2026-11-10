/** Family-4 reward updates: record rewards, one revision each. */

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>

#include "../../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../queuez/queuez_state_validation.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

namespace family4_datagen = middleware::datagen::family4;

// The grant array holds both change lists in full; State bounds each list separately.
static_assert(state::kRecordRewardGrantCapacity
              == family4_datagen::character::layout::kInventoryChangeRecordCapacity
                     + family4_datagen::account::layout::kProfileInventoryChangeRecordCapacity);

/** Resolves the item one granted row holds in the grant's after-image. */
[[nodiscard]] static bool
granted_row_definition(const state::AccountState& account,
                       std::size_t characterIndex,
                       const state::GrantedRow& row,
                       state::build_data::items::Definition& definition) noexcept {
    const auto serial = [&](const auto& held) noexcept {
        return held.mutationSerial == row.mutationSerial;
    };
    std::uint32_t definitionHash = 0;
    if (row.array == state::GrantedRowArray::profile) {
        const auto held = std::span(account.profileItems).first(account.profileItemCount);
        const auto found = std::find_if(held.begin(), held.end(), serial);
        definitionHash = found != held.end() ? found->definitionHash : 0;
    } else if (row.array == state::GrantedRowArray::characterItem) {
        const auto& items = account.characters[characterIndex].inventory;
        const auto held = std::span(items.values).first(items.count);
        const auto found = std::find_if(held.begin(), held.end(), serial);
        definitionHash = found != held.end() ? found->definitionHash : 0;
    } else {
        const auto& stacks = account.characters[characterIndex].stacks;
        const auto held = std::span(stacks.values).first(stacks.count);
        const auto found = std::find_if(held.begin(), held.end(), serial);
        definitionHash = found != held.end() ? found->definitionHash : 0;
    }
    return definitionHash != 0
           && state::build_data::find_item_definition_hash(definitionHash, definition);
}

/**
 * Names the granted rows of one encoded object in its change ring, which is still empty.
 * @param profileRows True for the account's profile rows, false for the character's rows.
 * @param items The object's encoded rows; each granted row must be exactly one of them.
 * @return Null on success, or the step that failed.
 */
template <typename ChangeList>
[[nodiscard]] static const char*
write_granted_changes(const state::AccountState& account,
                      const state::PendingRecordRewardGrant& mutation,
                      bool profileRows,
                      std::uint8_t changeKind,
                      std::span<const family4_datagen::inventory::layout::Entry> items,
                      ChangeList& changes) noexcept {
    std::size_t written = 0;
    for (std::size_t rowIndex = 0; rowIndex < mutation.rowCount; ++rowIndex) {
        const state::GrantedRow& granted = mutation.rows[rowIndex];
        if ((granted.array == state::GrantedRowArray::profile) != profileRows) {
            continue;
        }
        state::build_data::items::Definition definition{};
        if (written >= changes.records.size()
            || !granted_row_definition(account, mutation.characterIndex, granted, definition)) {
            return profileRows ? "record_reward_profile_definition"
                               : "record_reward_character_definition";
        }
        const auto matches =
            std::count_if(items.begin(), items.end(), [&](const auto& item) noexcept {
                return item.definitionIndex == definition.definitionIndex
                       && item.mutationSerial == granted.mutationSerial;
            });
        if (matches != 1) {
            return profileRows ? "record_reward_profile_row" : "record_reward_character_row";
        }
        auto& change = changes.records[written];
        change.sequence = static_cast<std::uint16_t>(written);
        change.mutationSerial = granted.mutationSerial;
        change.kind = changeKind;
        change.flags = static_cast<std::uint16_t>(
            change_flags(definition)
            | (state::newly_collected(mutation, granted, definition) ? kNewCollectibleChangeFlag
                                                                     : 0U));
        ++written;
    }
    changes.writeSlot = static_cast<std::uint16_t>(written);
    changes.nextSequence = static_cast<std::uint16_t>(written);
    return nullptr;
}

/** Builds all record rewards and the pending claim into one Family-4 revision. */
bool prepare_record_reward_grant(
    Scratch& scratch,
    const queuez::SessionState& before,
    const queuez::RecordRewardGrant& update,
    const state::PendingRecordRewardGrant& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept {
    namespace account_layout = family4_datagen::account::layout;
    namespace character_layout = family4_datagen::character::layout;
    if (!mutation.prepared || mutation.rewardCount > mutation.rewards.size()
        || !queuez::valid(before)
        || !queuez::valid(update.after) || !before.family4Active || before.family4ResidentCount == 0
        || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || update.accountSoid != mutation.accountSoid
        || update.characterSoid != mutation.characterSoid
        || update.accountSoid != before.family4RootSoid
        || update.after.family4Version != before.family4Version + 1
        || update.appendedResidentCount > state::kRecordRewardGrantCapacity
        || update.releasedResidentCount > update.releasedResidents.size()
        || update.releasedResidentCount > before.family4ResidentCount
        || update.after.family4ResidentCount
               != before.family4ResidentCount - update.releasedResidentCount
                      + update.appendedResidentCount
        || update.accountDefinitionId != before.family4Residents.front().definitionId
        || update.characterDefinitionId == 0 || update.itemInstanceDefinitionId == 0) {
        return report_failure("record_reward_session");
    }

    state::AccountState account{};
    Resolved selected{};
    // Both objects encode the grant's flags and quest starts, which State holds only after commit.
    state::unlocks::Table unlocks{};
    if (!state::preview_record_reward_grant(mutation, account)
        || !state::preview_reward_unlocks(mutation, unlocks)
        || mutation.characterIndex >= account.characterCount
        || account.primarySoid != update.accountSoid
        || account.characters[mutation.characterIndex].soid != update.characterSoid
        || !resolve(account, mutation.characterIndex, selected)
        || selected.characterObjectId != update.characterDefinitionId
        || selected.itemInstanceObjectId != update.itemInstanceDefinitionId) {
        return report_failure("record_reward_account");
    }

    // A claim moves existing items, which the client presents without counting an acquisition.
    const std::uint8_t changeKind =
        mutation.vendorTransfer ? kVendorTransferChangeKind : kChangeKind;
    state::GrantedResidents grantedResidents{};
    family4_datagen::loadout::ResolvedInstances residents{};
    if (!state::granted_residents(mutation, grantedResidents)) {
        return report_failure("record_reward_residents");
    }
    // State lists new character instances first, then new profile action sources.
    for (std::size_t index = 0; index < grantedResidents.count; ++index) {
        const std::uint64_t soid = grantedResidents.soids[index];
        if (residents.itemCount >= residents.items.size()) {
            return report_failure("record_reward_resident_capacity");
        }
        std::size_t matches = 0;
        for (std::size_t itemIndex = 0; itemIndex < selected.loadout.itemCount; ++itemIndex) {
            const auto& item = selected.loadout.items[itemIndex];
            if (item.instance.instanceSoid == soid) {
                if (item.equipped) {
                    return report_failure("record_reward_character_item");
                }
                residents.items[residents.itemCount] = {item.equipmentSlot, item.instance};
                ++matches;
            }
        }
        for (std::size_t profileIndex = 0; matches == 0 && profileIndex < account.profileItemCount;
             ++profileIndex) {
            if (account.profileItems[profileIndex].instanceSoid == soid) {
                if (!resolve_profile_item_instance(account.profileItems[profileIndex],
                                                   residents.items[residents.itemCount].instance)) {
                    return report_failure("record_reward_profile_instance");
                }
                ++matches;
            }
        }
        if (matches != 1) {
            return report_failure("record_reward_resident_identity");
        }
        ++residents.itemCount;
    }
    if (residents.itemCount != update.appendedResidentCount) {
        return report_failure("record_reward_resident_count");
    }
    for (std::size_t index = 0; index < residents.itemCount; ++index) {
        const auto& expected =
            update.after.family4Residents[before.family4ResidentCount - update.releasedResidentCount
                                          + index];
        if (expected.objectSoid != residents.items[index].instance.instanceSoid
            || expected.definitionId != update.itemInstanceDefinitionId) {
            return report_failure("record_reward_resident_order");
        }
    }
    // Held items whose values the grant changed, such as bounties a delivery advanced, are upserted
    // after the new residents. The manifest already lists them.
    family4_datagen::loadout::ResolvedInstances edited{};
    for (std::size_t itemIndex = 0; itemIndex < selected.loadout.itemCount; ++itemIndex) {
        const auto& item = selected.loadout.items[itemIndex];
        if (!item.equipped
            && state::item_values_edited(
                mutation.beforeCharacter, mutation.afterCharacter, item.instance.instanceSoid)) {
            edited.items[edited.itemCount++] = {item.equipmentSlot, item.instance};
        }
    }

    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("record_reward_reservation");
    }
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (account_layout::kObjectSize > rawStorage.size()
        || character_layout::kObjectSize > rawStorage.size()) {
        return report_failure("record_reward_storage");
    }

    Prepared staged{};
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t residentCursor = 0;
    if (!append_items(scratch,
                      rawStorage,
                      update.itemInstanceDefinitionId,
                      residents,
                      0,
                      staged,
                      residentCursor,
                      compressedExtent)
        || residentCursor != residents.itemCount) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_residents");
    }
    if (!append_items(scratch,
                      rawStorage,
                      update.itemInstanceDefinitionId,
                      edited,
                      0,
                      staged,
                      residentCursor,
                      compressedExtent)
        || residentCursor != residents.itemCount + edited.itemCount) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_edited_items");
    }

    const auto characterBytes = rawStorage.first(character_layout::kObjectSize);
    const state::CharacterState& character = account.characters[mutation.characterIndex];
    if (!family4_datagen::character::encode(
            character, selected.loadout, selected.lightEvaluation, characterBytes, unlocks)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_character_encode");
    }
    auto& characterObject = *reinterpret_cast<character_layout::Object*>(characterBytes.data());
    if (characterObject.inventoryChanges.writeSlot != 0
        || characterObject.inventoryChanges.nextSequence != 0
        || !std::all_of(characterObject.inventoryChanges.records.cbegin(),
                        characterObject.inventoryChanges.records.cend(),
                        kChangeRecordIsZero)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_character_changes");
    }
    const char* const characterRingFailure =
        write_granted_changes(account,
                              mutation,
                              false,
                              changeKind,
                              characterObject.inventoryItems,
                              characterObject.inventoryChanges);
    if (characterRingFailure != nullptr) {
        clear_after(scratch, reservation);
        return report_failure(characterRingFailure);
    }
    if (!apply_acquisition_presentation(
            characterBytes, selected.loadout, acquisitionPresentationRows)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_presentation");
    }
    if (!append_object(scratch,
                       characterBytes,
                       update.characterDefinitionId,
                       update.characterSoid,
                       staged.objects[residentCursor],
                       compressedExtent)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_character_object");
    }

    const auto accountBytes = rawStorage.first(account_layout::kObjectSize);
    if (!family4_datagen::account::encode(account, accountBytes, unlocks)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_account_encode");
    }
    auto& accountObject = *reinterpret_cast<account_layout::Object*>(accountBytes.data());
    if (accountObject.profileInventoryChanges.writeSlot != 0
        || accountObject.profileInventoryChanges.nextSequence != 0
        || !std::all_of(accountObject.profileInventoryChanges.records.cbegin(),
                        accountObject.profileInventoryChanges.records.cend(),
                        kChangeRecordIsZero)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_profile_changes");
    }
    const char* const profileRingFailure =
        write_granted_changes(account,
                              mutation,
                              true,
                              changeKind,
                              accountObject.profileItems,
                              accountObject.profileInventoryChanges);
    if (profileRingFailure != nullptr) {
        clear_after(scratch, reservation);
        return report_failure(profileRingFailure);
    }
    if (!append_object(scratch,
                       accountBytes,
                       update.accountDefinitionId,
                       update.accountSoid,
                       staged.objects[residentCursor + 1U],
                       compressedExtent)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_account_object");
    }

    std::size_t objectCount = residentCursor + 2U;
    for (const auto soid :
         std::span(update.releasedResidents).first(update.releasedResidentCount)) {
        if (objectCount == staged.objects.size()) {
            clear_after(scratch, reservation);
            return report_failure("record_reward_releases");
        }
        staged.objects[objectCount++] = {
            update.itemInstanceDefinitionId, soid, middleware::queuez::Encoding::oodle, {}};
    }

    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset
                       + (std::max)(account_layout::kObjectSize, character_layout::kObjectSize));
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        queuez::kAccountFamilyType,
        update.accountSoid,
        update.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_commit");
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
