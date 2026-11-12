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
 * Names the granted rows of one encoded object in its change ring, which must still be empty.
 * @param profileRows True for the account's profile rows, false for the character's rows.
 * @param items The object's encoded rows; each granted row must be exactly one of them.
 * @return Null on success, or the step that failed.
 */
template <typename ChangeList>
[[nodiscard]] static const char*
write_granted_changes(const state::AccountState& account,
                      const state::PendingRecordRewardGrant& mutation,
                      bool profileRows,
                      std::span<const family4_datagen::inventory::layout::Entry> items,
                      ChangeList& changes) noexcept {
    if (changes.writeSlot != 0 || changes.nextSequence != 0
        || !std::all_of(changes.records.cbegin(), changes.records.cend(), kChangeRecordIsZero)) {
        return profileRows ? "record_reward_profile_changes" : "record_reward_character_changes";
    }
    // A claim moves existing items, which the client presents without counting an acquisition.
    const std::uint8_t changeKind =
        mutation.vendorTransfer ? kVendorTransferChangeKind : kChangeKind;
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

/** @return True when the staged update is one revision past the session and names the grant. */
[[nodiscard]] static bool
grant_follows_session(const queuez::SessionState& before,
                      const queuez::RecordRewardGrant& update,
                      const state::PendingRecordRewardGrant& mutation) noexcept {
    return mutation.prepared && mutation.rewardCount <= mutation.rewards.size()
           && queuez::valid(before) && queuez::valid(update.after) && before.family4Active
           && before.family4ResidentCount != 0
           && before.family4Version != (std::numeric_limits<std::int32_t>::max)()
           && update.accountSoid == mutation.accountSoid
           && update.characterSoid == mutation.characterSoid
           && update.accountSoid == before.family4RootSoid
           && update.after.family4Version == before.family4Version + 1
           && update.appendedResidentCount <= state::kRecordRewardGrantCapacity
           && update.releasedResidentCount <= update.releasedResidents.size()
           && update.releasedResidentCount <= before.family4ResidentCount
           && update.after.family4ResidentCount
                  == before.family4ResidentCount - update.releasedResidentCount
                         + update.appendedResidentCount
           && update.accountDefinitionId == before.family4Residents.front().definitionId
           && update.characterDefinitionId != 0 && update.itemInstanceDefinitionId != 0;
}

/**
 * Resolves each instance the grant adds and checks it against the staged manifest's new tail.
 * @param residents Receives the new residents in State's order: character instances, then profile
 *        action sources.
 * @return Null on success, or the step that failed.
 */
[[nodiscard]] static const char*
resolve_granted_residents(const state::AccountState& account,
                          const Resolved& selected,
                          const queuez::SessionState& before,
                          const queuez::RecordRewardGrant& update,
                          const state::PendingRecordRewardGrant& mutation,
                          family4_datagen::loadout::ResolvedInstances& residents) noexcept {
    state::GrantedResidents grantedResidents{};
    if (!state::granted_residents(mutation, grantedResidents)) {
        return "record_reward_residents";
    }
    for (std::size_t index = 0; index < grantedResidents.count; ++index) {
        const std::uint64_t soid = grantedResidents.soids[index];
        if (residents.itemCount >= residents.items.size()) {
            return "record_reward_resident_capacity";
        }
        std::size_t matches = 0;
        for (std::size_t itemIndex = 0; itemIndex < selected.loadout.itemCount; ++itemIndex) {
            const auto& item = selected.loadout.items[itemIndex];
            if (item.instance.instanceSoid == soid) {
                if (item.equipped) {
                    return "record_reward_character_item";
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
                    return "record_reward_profile_instance";
                }
                ++matches;
            }
        }
        if (matches != 1) {
            return "record_reward_resident_identity";
        }
        ++residents.itemCount;
    }
    if (residents.itemCount != update.appendedResidentCount) {
        return "record_reward_resident_count";
    }
    for (std::size_t index = 0; index < residents.itemCount; ++index) {
        const auto& expected =
            update.after.family4Residents[before.family4ResidentCount - update.releasedResidentCount
                                          + index];
        if (expected.objectSoid != residents.items[index].instance.instanceSoid
            || expected.definitionId != update.itemInstanceDefinitionId) {
            return "record_reward_resident_order";
        }
    }
    return nullptr;
}

/**
 * Encodes the selected character's after-image, names the granted rows in its change ring and
 * pins the rows an earlier acquisition presents.
 * @return Null on success, or the step that failed.
 */
[[nodiscard]] static const char*
encode_granted_character(const state::AccountState& account,
                         const state::unlocks::Table& unlocks,
                         const state::PendingRecordRewardGrant& mutation,
                         const Resolved& selected,
                         std::span<const queuez::AcquisitionPresentationRow> presentationRows,
                         std::span<std::byte> characterBytes) noexcept {
    if (!family4_datagen::character::encode(account.characters[mutation.characterIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            characterBytes,
                                            unlocks)) {
        return "record_reward_character_encode";
    }
    auto& characterObject =
        *reinterpret_cast<family4_datagen::character::layout::Object*>(characterBytes.data());
    const char* const ringFailure = write_granted_changes(
        account, mutation, false, characterObject.inventoryItems, characterObject.inventoryChanges);
    if (ringFailure != nullptr) {
        return ringFailure;
    }
    return apply_acquisition_presentation(characterBytes, selected.loadout, presentationRows)
               ? nullptr
               : "record_reward_presentation";
}

/**
 * Encodes the account's after-image and names the granted profile rows in its change ring.
 * @return Null on success, or the step that failed.
 */
[[nodiscard]] static const char*
encode_granted_account(const state::AccountState& account,
                       const state::unlocks::Table& unlocks,
                       const state::PendingRecordRewardGrant& mutation,
                       std::span<std::byte> accountBytes) noexcept {
    family4_datagen::account::StateFields accountFields{};
    if (!read_account_state_fields(account, accountFields)
        || !family4_datagen::account::encode(account, accountFields, accountBytes, unlocks)) {
        return "record_reward_account_encode";
    }
    auto& accountObject =
        *reinterpret_cast<family4_datagen::account::layout::Object*>(accountBytes.data());
    return write_granted_changes(account,
                                 mutation,
                                 true,
                                 accountObject.profileItems,
                                 accountObject.profileInventoryChanges);
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
    if (!grant_follows_session(before, update, mutation)) {
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

    family4_datagen::loadout::ResolvedInstances residents{};
    const char* const residentFailure =
        resolve_granted_residents(account, selected, before, update, mutation, residents);
    if (residentFailure != nullptr) {
        return report_failure(residentFailure);
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

    // Every failure past the reservation clears what this grant staged behind it.
    const auto fail = [&scratch, &reservation](const char* step) noexcept {
        clear_after(scratch, reservation);
        return report_failure(step);
    };
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
        return fail("record_reward_residents");
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
        return fail("record_reward_edited_items");
    }

    const auto characterBytes = rawStorage.first(character_layout::kObjectSize);
    const char* const characterFailure = encode_granted_character(
        account, unlocks, mutation, selected, acquisitionPresentationRows, characterBytes);
    if (characterFailure != nullptr) {
        return fail(characterFailure);
    }
    if (!append_object(scratch,
                       characterBytes,
                       update.characterDefinitionId,
                       update.characterSoid,
                       staged.objects[residentCursor],
                       compressedExtent)) {
        return fail("record_reward_character_object");
    }

    const auto accountBytes = rawStorage.first(account_layout::kObjectSize);
    const char* const accountFailure =
        encode_granted_account(account, unlocks, mutation, accountBytes);
    if (accountFailure != nullptr) {
        return fail(accountFailure);
    }
    if (!append_object(scratch,
                       accountBytes,
                       update.accountDefinitionId,
                       update.accountSoid,
                       staged.objects[residentCursor + 1U],
                       compressedExtent)) {
        return fail("record_reward_account_object");
    }

    std::size_t objectCount = residentCursor + 2U;
    for (const auto soid :
         std::span(update.releasedResidents).first(update.releasedResidentCount)) {
        if (objectCount == staged.objects.size()) {
            return fail("record_reward_releases");
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
        return fail("record_reward_commit");
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
