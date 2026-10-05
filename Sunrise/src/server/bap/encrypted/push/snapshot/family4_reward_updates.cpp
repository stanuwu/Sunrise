/** Family-4 reward updates: season pass packages and record rewards, one revision each. */

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>

#include "../../../../../middleware/datagen/definitions.h"
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

// Every row one reward grant publishes is named in the character or the profile change list.
static_assert(state::kRecordRewardGrantCapacity
              == family4_datagen::character::layout::kInventoryChangeRecordCapacity
                     + family4_datagen::account::layout::kProfileInventoryChangeRecordCapacity);

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
    if (!mutation.prepared || mutation.rewardCount == 0
        || mutation.rewardCount > mutation.rewards.size() || !queuez::valid(before)
        || !queuez::valid(update.after) || !before.family4Active || before.family4ResidentCount == 0
        || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || update.accountSoid != mutation.accountSoid
        || update.characterSoid != mutation.characterSoid
        || update.accountSoid != before.family4RootSoid
        || update.after.family4Version != before.family4Version + 1
        || update.appendedResidentCount > mutation.rewardCount
        || update.after.family4ResidentCount
               != before.family4ResidentCount + update.appendedResidentCount
        || update.accountDefinitionId != before.family4Residents.front().definitionId
        || update.characterDefinitionId == 0 || update.itemInstanceDefinitionId == 0) {
        return report_failure("record_reward_session");
    }

    state::AccountState account{};
    Resolved selected{};
    if (!state::preview_record_reward_grant(mutation, account)
        || mutation.characterIndex >= account.characterCount
        || account.primarySoid != update.accountSoid
        || account.characters[mutation.characterIndex].soid != update.characterSoid
        || !resolve(account, mutation.characterIndex, selected)
        || selected.characterObjectId != update.characterDefinitionId
        || selected.itemInstanceObjectId != update.itemInstanceDefinitionId) {
        return report_failure("record_reward_account");
    }

    family4_datagen::loadout::ResolvedInstances residents{};
    for (std::size_t rewardIndex = 0; rewardIndex < mutation.rewardCount; ++rewardIndex) {
        const state::PreparedRecordReward& reward = mutation.rewards[rewardIndex];
        if (reward.kind == state::RecordRewardKind::characterInstance) {
            std::size_t matches = 0;
            for (std::size_t itemIndex = 0; itemIndex < selected.loadout.itemCount; ++itemIndex) {
                const auto& item = selected.loadout.items[itemIndex];
                if (item.instance.instanceSoid != reward.instanceSoid) {
                    continue;
                }
                if (item.equipped || item.inventoryRow != reward.inventoryRow
                    || item.mutationSerial != reward.mutationSerial) {
                    return report_failure("record_reward_character_item");
                }
                residents.items[residents.itemCount++] = {item.equipmentSlot, item.instance};
                ++matches;
            }
            if (matches != 1) {
                return report_failure("record_reward_character_instance");
            }
        } else if (reward.kind == state::RecordRewardKind::profileStack
                   && reward.appendedProfileResident) {
            if (reward.stateIndex >= account.profileItemCount
                || residents.itemCount >= residents.items.size()
                || !resolve_profile_item_instance(account.profileItems[reward.stateIndex],
                                                  residents.items[residents.itemCount].instance)) {
                return report_failure("record_reward_profile_instance");
            }
            ++residents.itemCount;
        } else if (reward.kind == state::RecordRewardKind::profileStack
                   && reward.instanceSoid != 0) {
            std::size_t matches = 0;
            for (std::size_t residentIndex = 0; residentIndex < before.family4ResidentCount;
                 ++residentIndex) {
                const auto& resident = before.family4Residents[residentIndex];
                matches += static_cast<std::size_t>(resident.objectSoid == reward.instanceSoid
                                                    && resident.definitionId
                                                           == update.itemInstanceDefinitionId);
            }
            if (matches != 1) {
                return report_failure("record_reward_profile_resident");
            }
        }
    }
    if (residents.itemCount != update.appendedResidentCount) {
        return report_failure("record_reward_resident_count");
    }
    for (std::size_t index = 0; index < residents.itemCount; ++index) {
        const auto& expected = update.after.family4Residents[before.family4ResidentCount + index];
        if (expected.objectSoid != residents.items[index].instance.instanceSoid
            || expected.definitionId != update.itemInstanceDefinitionId) {
            return report_failure("record_reward_resident_order");
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

    const auto characterBytes = rawStorage.first(character_layout::kObjectSize);
    const state::CharacterState& character = account.characters[mutation.characterIndex];
    state::unlocks::Table unlocks{};
    if (!state::preview_reward_unlocks(mutation, unlocks)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_unlocks");
    }
    // Both objects must publish the same reward after-image, including spent claim credit.
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
    std::size_t characterChanges = 0;
    for (std::size_t rewardIndex = 0; rewardIndex < mutation.rewardCount; ++rewardIndex) {
        const state::PreparedRecordReward& reward = mutation.rewards[rewardIndex];
        if (reward.kind == state::RecordRewardKind::profileStack
            || reward.kind == state::RecordRewardKind::accountUnlock) {
            continue;
        }
        state::build_data::items::Definition definition{};
        if (characterChanges >= characterObject.inventoryChanges.records.size()
            || !state::build_data::find_item_definition_hash(reward.definitionHash, definition)) {
            clear_after(scratch, reservation);
            return report_failure("record_reward_character_definition");
        }
        std::size_t found = characterObject.inventoryItems.size();
        for (std::size_t row = 0; row < characterObject.inventoryItems.size(); ++row) {
            const auto& item = characterObject.inventoryItems[row];
            const bool matches = reward.kind == state::RecordRewardKind::characterInstance
                                     ? item.instanceSoid == reward.instanceSoid
                                     : item.instanceSoid == 0
                                           && item.definitionIndex == definition.definitionIndex
                                           && item.mutationSerial == reward.mutationSerial;
            if (!matches) {
                continue;
            }
            if (found != characterObject.inventoryItems.size()
                || item.quantity != reward.afterQuantity) {
                clear_after(scratch, reservation);
                return report_failure("record_reward_character_row");
            }
            found = row;
        }
        if (found == characterObject.inventoryItems.size()) {
            clear_after(scratch, reservation);
            return report_failure("record_reward_character_row_missing");
        }
        auto& change = characterObject.inventoryChanges.records[characterChanges];
        change.sequence = static_cast<std::uint16_t>(characterChanges);
        change.mutationSerial = reward.mutationSerial;
        change.kind = kChangeKind;
        change.flags = kChangeFlags;
        ++characterChanges;
    }
    characterObject.inventoryChanges.writeSlot = static_cast<std::uint16_t>(characterChanges);
    characterObject.inventoryChanges.nextSequence = static_cast<std::uint16_t>(characterChanges);
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
    std::size_t profileChanges = 0;
    for (std::size_t rewardIndex = 0; rewardIndex < mutation.rewardCount; ++rewardIndex) {
        const state::PreparedRecordReward& reward = mutation.rewards[rewardIndex];
        if (reward.kind != state::RecordRewardKind::profileStack) {
            continue;
        }
        state::build_data::items::Definition definition{};
        if (profileChanges >= accountObject.profileInventoryChanges.records.size()
            || !state::build_data::find_item_definition_hash(reward.definitionHash, definition)) {
            clear_after(scratch, reservation);
            return report_failure("record_reward_profile_definition");
        }
        std::size_t found = accountObject.profileItems.size();
        for (std::size_t row = 0; row < accountObject.profileItems.size(); ++row) {
            const auto& item = accountObject.profileItems[row];
            if (item.definitionIndex != definition.definitionIndex
                || item.mutationSerial != reward.mutationSerial) {
                continue;
            }
            if (found != accountObject.profileItems.size()
                || item.quantity != reward.afterQuantity) {
                clear_after(scratch, reservation);
                return report_failure("record_reward_profile_row");
            }
            found = row;
        }
        if (found == accountObject.profileItems.size()) {
            clear_after(scratch, reservation);
            return report_failure("record_reward_profile_row_missing");
        }
        auto& change = accountObject.profileInventoryChanges.records[profileChanges];
        change.sequence = static_cast<std::uint16_t>(profileChanges);
        change.mutationSerial = reward.mutationSerial;
        change.kind = kChangeKind;
        change.flags = kChangeFlags;
        ++profileChanges;
    }
    accountObject.profileInventoryChanges.writeSlot = static_cast<std::uint16_t>(profileChanges);
    accountObject.profileInventoryChanges.nextSequence = static_cast<std::uint16_t>(profileChanges);
    if (!append_object(scratch,
                       accountBytes,
                       update.accountDefinitionId,
                       update.accountSoid,
                       staged.objects[residentCursor + 1U],
                       compressedExtent)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_account_object");
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
        std::span(staged.objects).first(residentCursor + 2U),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("record_reward_commit");
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
