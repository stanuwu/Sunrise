#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>

#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/account/selection_patch/\
account_selection_patch_encoder.h"
#include "../../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../../middleware/datagen/family4/instance/instance_encoder.h"
#include "../../../../../middleware/datagen/family4/instance/layout.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

namespace family4_datagen = middleware::datagen::family4;
namespace selection_patch = middleware::datagen::family4::account::selection_patch;

/**
 * Moves each newly acquired item to the inventory row the acquisition presentation names.
 * @param characterBytes Character object to edit in place; must hold a whole object.
 * @param loadout Resolved items the presentation rows refer to by instance soid.
 * @return False when a row is claimed twice, out of range, or names no resolved item.
 */
bool apply_acquisition_presentation(
    std::span<std::byte> characterBytes,
    const family4_datagen::loadout::ResolvedLoadout& loadout,
    std::span<const queuez::AcquisitionPresentationRow> presentationRows) noexcept {
    namespace character_layout = family4_datagen::character::layout;
    // One new-item flag byte covers 8 consecutive inventory rows.
    constexpr std::size_t kBitsPerFlagByte = 8;
    // Every bit set is the native empty biased definition index.
    constexpr std::uint16_t kEmptyDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();
    if (presentationRows.empty()) {
        return true;
    }
    if (characterBytes.size() < character_layout::kObjectSize
        || presentationRows.size() > queuez::kAcquisitionPresentationRowCapacity) {
        return false;
    }

    auto& characterObject = *reinterpret_cast<character_layout::Object*>(characterBytes.data());
    std::array<const family4_datagen::loadout::ResolvedItem*,
               queuez::kAcquisitionPresentationRowCapacity>
        resolvedItems{};
    std::array<bool, character_layout::kInventoryCapacity> occupiedRows{};
    for (std::size_t presentationIndex = 0; presentationIndex < presentationRows.size();
         ++presentationIndex) {
        const auto& presentation = presentationRows[presentationIndex];
        if (presentation.instanceSoid == 0
            || presentation.inventoryRow >= characterObject.inventoryItems.size()
            || occupiedRows[presentation.inventoryRow]) {
            return false;
        }
        occupiedRows[presentation.inventoryRow] = true;
        for (std::size_t itemIndex = 0; itemIndex < loadout.itemCount; ++itemIndex) {
            const auto& item = loadout.items[itemIndex];
            if (item.instance.instanceSoid != presentation.instanceSoid) {
                continue;
            }
            if (resolvedItems[presentationIndex] != nullptr
                || item.inventoryRow >= characterObject.inventoryItems.size()) {
                return false;
            }
            resolvedItems[presentationIndex] = &item;
        }
        if (resolvedItems[presentationIndex] == nullptr) {
            return false;
        }
    }

    for (const auto* item : std::span(resolvedItems).first(presentationRows.size())) {
        const std::size_t row = item->inventoryRow;
        characterObject.inventoryItems[row] = {};
        characterObject.inventoryItems[row].definitionIndex = kEmptyDefinitionIndex;
        characterObject.newItemFlags[row / kBitsPerFlagByte] &=
            ~(std::byte{1U} << (row % kBitsPerFlagByte));
        characterObject.instanceProgressWatermarks[row] = 0;
    }
    for (std::size_t presentationIndex = 0; presentationIndex < presentationRows.size();
         ++presentationIndex) {
        const auto& presentation = presentationRows[presentationIndex];
        const auto* item = resolvedItems[presentationIndex];
        auto& row = characterObject.inventoryItems[presentation.inventoryRow];
        if (row.definitionIndex != kEmptyDefinitionIndex) {
            return false;
        }
        row.definitionIndex = item->instance.baseDefinitionIndex;
        row.instanceSoid = item->instance.instanceSoid;
        row.quantity = item->quantity;
        row.mutationSerial = item->mutationSerial;
        row.flags = item->flags;
        if (!item->seen) {
            characterObject.newItemFlags[presentation.inventoryRow / kBitsPerFlagByte] |=
                std::byte{1U} << (presentation.inventoryRow % kBitsPerFlagByte);
        }
        characterObject.instanceProgressWatermarks[presentation.inventoryRow] = 1;
    }
    return true;
}

/** Builds the Family-4 increment that moves the character object to the picked character. */
bool prepare_selection_move(Scratch& scratch,
                            const queuez::SelectCharacter& select,
                            Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("move_reservation");
    }
    const state::AccountState account = state::account_snapshot();
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    Resolved selected{};
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || !resolve(account, *selectedIndex, selected)
        || account.characters[selected.characterIndex].soid != select.selectedCharacterSoid) {
        return report_failure("move_selection");
    }

    Prepared staged{};
    staged.rawClearSize = reservation.rawClearSize;
    staged.compressedClearSize = reservation.compressedClearSize;
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t objectCount = 0;
    // A first select resends the character even when it names the current one. A pending change
    // may skip the resend only when the character did not move.
    if (!select.patchAccount || select.previousCharacterSoid != select.selectedCharacterSoid) {
        // A zero previous key means there is no published character slot to release.
        if (select.previousCharacterSoid != 0) {
            // TODO: a release should name no codec. This one names oodle and the family-zero
            // release names none; which form the client requires is not established.
            staged.objects[objectCount++] = middleware::queuez::Object{
                select.characterDefinitionId,
                select.previousCharacterSoid,
                middleware::queuez::Encoding::oodle,
                {},
            };
        }
        if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
            return report_failure("move_character_storage");
        }
        const auto characterBytes =
            rawStorage.first(family4_datagen::character::layout::kObjectSize);
        if (!family4_datagen::character::encode(account.characters[selected.characterIndex],
                                                selected.loadout,
                                                selected.lightEvaluation,
                                                characterBytes)) {
            return report_failure("move_character_object");
        }
        if (!append_object(scratch,
                           characterBytes,
                           select.characterDefinitionId,
                           select.selectedCharacterSoid,
                           staged.objects[objectCount++],
                           compressedExtent)) {
            return report_failure("move_character_object");
        }
        staged.rawClearSize = (std::max)(staged.rawClearSize,
                                         reservation.rawWriteOffset
                                             + family4_datagen::character::layout::kObjectSize);
    }

    // The compressed character no longer occupies the raw span needed for the account body.
    if (select.patchAccount) {
        std::size_t patchSize = 0;
        if (!selection_patch::encode(select.selectedCharacterSoid, rawStorage, patchSize)
            || patchSize != selection_patch::kPayloadSize) {
            return report_failure("move_account_patch");
        }
        staged.objects[objectCount++] = middleware::queuez::Object{
            select.accountDefinitionId,
            select.after.family4RootSoid,
            middleware::queuez::Encoding::tagReflection,
            rawStorage.first(patchSize),
        };
        staged.rawClearSize =
            (std::max)(staged.rawClearSize, reservation.rawWriteOffset + patchSize);
    } else {
        if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
            return report_failure("move_account_storage");
        }
        const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
        if (!family4_datagen::account::encode(account, accountBytes)) {
            return report_failure("move_account_object");
        }
        if (!append_object(scratch,
                           accountBytes,
                           select.accountDefinitionId,
                           select.after.family4RootSoid,
                           staged.objects[objectCount++],
                           compressedExtent)) {
            return report_failure("move_account_object");
        }
        staged.rawClearSize =
            (std::max)(staged.rawClearSize,
                       reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        select.after.family4RootSoid,
        select.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("move_commit");
    }
    return true;
}

/** Compresses and commits one selected-character-only Family-4 increment. */
[[nodiscard]] bool finish_character_upsert(Scratch& scratch,
                                           const Reservation& reservation,
                                           std::span<std::byte> characterBytes,
                                           const queuez::EquipmentSwap& update,
                                           const char* objectFailure,
                                           const char* commitFailure,
                                           Prepared& prepared) noexcept {
    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::character::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    if (!append_object(scratch,
                       characterBytes,
                       update.characterDefinitionId,
                       update.characterSoid,
                       staged.objects.front(),
                       compressedExtent)) {
        return report_failure(objectFailure);
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{kAccountFamilyType,
                                               update.after.family4RootSoid,
                                               update.after.family4Version,
                                               0,
                                               std::span(staged.objects).first(1)};
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure(commitFailure);
    }
    return true;
}

/** Builds a single-character Family-4 upsert from an uncommitted equipment after-image. */
bool prepare_equipment_swap(
    Scratch& scratch,
    const queuez::EquipmentSwap& swap,
    const state::PendingEquipmentSwap& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("equip_reservation");
    }
    state::AccountState account = state::account_snapshot();
    if (!mutation.prepared || mutation.characterSoid == 0
        || mutation.characterSoid != swap.characterSoid
        || mutation.characterIndex >= account.characterCount
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("equip_mutation");
    }
    account.characters[mutation.characterIndex] = mutation.afterCharacter;
    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.characterObjectId != swap.characterDefinitionId) {
        return report_failure("equip_selection");
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
        return report_failure("equip_character_storage");
    }
    const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account.characters[mutation.characterIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            characterBytes)) {
        return report_failure("equip_character_object");
    }

    // An equipment update must not present the displaced item as another acquisition.
    constexpr std::size_t kBitsPerNewItemFlagByte = 8;
    const std::uint64_t movedIntoInventory = mutation.kind == state::EquipmentMutationKind::equip
                                                 ? mutation.previousInstanceSoid
                                                 : mutation.requestedInstanceSoid;
    auto& characterObject =
        *reinterpret_cast<family4_datagen::character::layout::Object*>(characterBytes.data());

    if (!apply_acquisition_presentation(
            characterBytes, selected.loadout, acquisitionPresentationRows)) {
        clear_after(scratch, reservation);
        return report_failure("equip_presentation");
    }

    std::size_t movedRowMatches = 0;
    if (movedIntoInventory != 0) {
        for (std::size_t row = 0; row < characterObject.inventoryItems.size(); ++row) {
            if (characterObject.inventoryItems[row].instanceSoid != movedIntoInventory) {
                continue;
            }
            const std::size_t flagByte = row / kBitsPerNewItemFlagByte;
            const std::byte flagMask = std::byte{1U} << (row % kBitsPerNewItemFlagByte);
            characterObject.newItemFlags[flagByte] &= ~flagMask;
            ++movedRowMatches;
        }
        if (movedRowMatches != 1) {
            clear_after(scratch, reservation);
            return report_failure("equip_moved_inventory_row");
        }
    }
    return finish_character_upsert(scratch,
                                   reservation,
                                   characterBytes,
                                   swap,
                                   "equip_character_object",
                                   "equip_commit",
                                   prepared);
}

/** Builds a single-character Family-4 upsert from an uncommitted item-state after-image. */
bool prepare_item_state(
    Scratch& scratch,
    const queuez::EquipmentSwap& update,
    const state::PendingItemState& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("item_state_reservation");
    }
    state::AccountState account = state::account_snapshot();
    if (!mutation.prepared || mutation.characterSoid == 0
        || mutation.characterSoid != update.characterSoid
        || mutation.characterIndex >= account.characterCount
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("item_state_mutation");
    }
    account.characters[mutation.characterIndex] = mutation.afterCharacter;
    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.characterObjectId != update.characterDefinitionId) {
        return report_failure("item_state_selection");
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
        return report_failure("item_state_character_storage");
    }
    const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account.characters[mutation.characterIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            characterBytes)) {
        return report_failure("item_state_character_object");
    }
    const auto& encoded =
        *reinterpret_cast<const family4_datagen::character::layout::Object*>(characterBytes.data());
    if (mutation.itemIndex >= mutation.afterCharacter.inventory.count && !mutation.targetEquipped) {
        return report_failure("item_state_inventory_index");
    }
    std::size_t matchingRows = 0;
    for (const auto& row : encoded.inventoryItems) {
        matchingRows +=
            static_cast<std::size_t>(row.instanceSoid == mutation.targetInstanceSoid
                                     && row.definitionIndex == mutation.targetDefinitionIndex
                                     && row.flags == mutation.afterFlags);
    }
    if (matchingRows != 1) {
        return report_failure("item_state_character_shape");
    }
    if (!apply_acquisition_presentation(
            characterBytes, selected.loadout, acquisitionPresentationRows)) {
        clear_after(scratch, reservation);
        return report_failure("item_state_presentation");
    }

    return finish_character_upsert(scratch,
                                   reservation,
                                   characterBytes,
                                   update,
                                   "item_state_character_object",
                                   "item_state_commit",
                                   prepared);
}

/** Builds a single-character Family-4 upsert from an uncommitted artifact mask. */
bool prepare_artifact_purchase(
    Scratch& scratch,
    const queuez::EquipmentSwap& update,
    const state::PendingArtifactPurchase& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("artifact_reservation");
    }
    const state::AccountState account = state::account_snapshot();
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.accountSoid != account.primarySoid
        || mutation.characterSoid != update.characterSoid
        || mutation.characterIndex >= account.characterCount
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid
        || !account.characters[mutation.characterIndex].selected) {
        return report_failure("artifact_mutation");
    }
    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.characterObjectId != update.characterDefinitionId) {
        return report_failure("artifact_selection");
    }
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
        return report_failure("artifact_character_storage");
    }
    const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account.characters[mutation.characterIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            characterBytes)) {
        return report_failure("artifact_character_object");
    }
    if (!apply_acquisition_presentation(
            characterBytes, selected.loadout, acquisitionPresentationRows)) {
        clear_after(scratch, reservation);
        return report_failure("artifact_projection");
    }

    return finish_character_upsert(scratch,
                                   reservation,
                                   characterBytes,
                                   update,
                                   "artifact_character_object",
                                   "artifact_commit",
                                   prepared);
}

/** Builds an incremental reset image without re-announcing every resident item. */
bool prepare_artifact_reset(Scratch& scratch,
                            const queuez::EquipmentSwap& update,
                            Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    const state::AccountState account = state::account_snapshot();
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    Resolved selected{};
    std::uint32_t accountDefinitionId = 0;
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || !resolve(account, *selectedIndex, selected)
        || !middleware::datagen::object_id(
            kAccountFamilyType, middleware::datagen::kAccountSlot, accountDefinitionId)
        || account.primarySoid != update.after.family4RootSoid
        || account.characters[*selectedIndex].soid != update.characterSoid
        || selected.characterObjectId != update.characterDefinitionId) {
        return report_failure("artifact_reset_state");
    }
    bool accountResident = false;
    for (std::size_t index = 0; index < update.after.family4ResidentCount; ++index) {
        const auto& resident = update.after.family4Residents[index];
        accountResident = accountResident
                          || (resident.definitionId == accountDefinitionId
                              && resident.objectSoid == account.primarySoid);
    }
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    const std::size_t required = (std::max)(family4_datagen::account::layout::kObjectSize,
                                            family4_datagen::character::layout::kObjectSize);
    if (!accountResident || required > rawStorage.size()) {
        return report_failure("artifact_reset_storage");
    }

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize, reservation.rawWriteOffset + required);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
    if (!family4_datagen::account::encode(account, accountBytes)
        || !append_object(scratch,
                          accountBytes,
                          accountDefinitionId,
                          account.primarySoid,
                          staged.objects[0],
                          compressedExtent)) {
        clear_after(scratch, reservation);
        return report_failure("artifact_reset_account");
    }
    const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account.characters[*selectedIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            characterBytes)
        || !append_object(scratch,
                          characterBytes,
                          update.characterDefinitionId,
                          update.characterSoid,
                          staged.objects[1],
                          compressedExtent)) {
        clear_after(scratch, reservation);
        return report_failure("artifact_reset_character");
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{kAccountFamilyType,
                                               update.after.family4RootSoid,
                                               update.after.family4Version,
                                               0,
                                               std::span(staged.objects).first(2)};
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("artifact_reset_commit");
    }
    return true;
}

/** Builds one exact current item-resident upsert after artifact reset. */
bool prepare_artifact_item_refresh(Scratch& scratch,
                                   const queuez::EquipmentSwap& update,
                                   std::uint64_t instanceSoid,
                                   Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    const state::AccountState account = state::account_snapshot();
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    Resolved selected{};
    if (instanceSoid == 0 || !state::account::valid(account) || !selectedIndex.has_value()
        || !resolve(account, *selectedIndex, selected)
        || selected.characterObjectId != update.characterDefinitionId
        || update.characterSoid != account.characters[*selectedIndex].soid) {
        return report_failure("artifact_item_refresh_selection");
    }
    family4_datagen::loadout::ResolvedInstances changed{};
    for (const auto& item : selected.loadout.items) {
        if (item.instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (changed.itemCount != 0) {
            return report_failure("artifact_item_refresh_duplicate");
        }
        changed.items[changed.itemCount++] = {item.equipmentSlot, item.instance};
    }
    if (changed.itemCount != 1) {
        return report_failure("artifact_item_refresh_missing");
    }
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    Prepared staged{};
    std::size_t itemCursor = 0;
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    if (!append_items(scratch,
                      rawStorage,
                      selected.itemInstanceObjectId,
                      changed,
                      0,
                      staged,
                      itemCursor,
                      compressedExtent)
        || itemCursor != 1) {
        clear_after(scratch, reservation);
        return report_failure("artifact_item_refresh_encode");
    }
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::instance::layout::kObjectSize);
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{kAccountFamilyType,
                                               update.after.family4RootSoid,
                                               update.after.family4Version,
                                               0,
                                               std::span(staged.objects).first(1)};
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("artifact_item_refresh_commit");
    }
    return true;
}

bool prepare_postmaster_claim(Scratch& scratch,
                              const queuez::EquipmentSwap& update,
                              const state::PendingPostmasterClaim& mutation,
                              Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("postmaster_reservation");
    }
    auto account = std::unique_ptr<state::AccountState>{new (std::nothrow) state::AccountState};
    if (!account || !state::preview_postmaster_claim(mutation, *account)
        || mutation.characterSoid != update.characterSoid
        || account->primarySoid != update.after.family4RootSoid) {
        return report_failure("postmaster_mutation");
    }
    Resolved selected{};
    const auto selectedIndex = find_character_index(*account);
    if (!selectedIndex || *selectedIndex != mutation.characterIndex
        || !resolve(*account, mutation.characterIndex, selected)
        || selected.characterObjectId != update.characterDefinitionId) {
        return report_failure("postmaster_selection");
    }
    const auto raw = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (raw.size() < family4_datagen::character::layout::kObjectSize) {
        return report_failure("postmaster_storage");
    }
    const auto bytes = raw.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account->characters[mutation.characterIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            bytes)) {
        return report_failure("postmaster_character");
    }
    // No acquisition overlay: the source's old Lost Items row must become empty in this revision.
    return finish_character_upsert(
        scratch, reservation, bytes, update, "postmaster_object", "postmaster_snapshot", prepared);
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
