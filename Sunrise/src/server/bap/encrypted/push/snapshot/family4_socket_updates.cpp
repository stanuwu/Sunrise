/**
 * Family-4 socket and subclass updates: one changed item instance, any held items a bake advanced,
 * any charged account, and the character whose rows a bake gave new serials.
 */

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>

#include "../../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../../middleware/datagen/family4/instance/instance_encoder.h"
#include "../../../../../middleware/datagen/family4/instance/layout.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

namespace family4_datagen = middleware::datagen::family4;

/**
 * Builds a resident item upsert followed by the account when the cost consumes or an exchange
 * pays, whose change ring names each credited row and whose banks carry the action's unlock rows.
 * Held items a bake advanced follow the target. A bake's character follows the account, carrying
 * the new serials of the oven and those items.
 */
bool prepare_socket_plug(Scratch& scratch,
                         const queuez::SocketPlug& socketPlug,
                         const state::PendingSocketPlug& mutation,
                         Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("socket_plug_reservation");
    }

    state::AccountState account{};
    state::unlocks::Table unlocks{};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.targetInstanceSoid == 0 || mutation.accountSoid != socketPlug.accountSoid
        || mutation.characterSoid != socketPlug.characterSoid
        || mutation.targetInstanceSoid != socketPlug.targetInstanceSoid
        || mutation.profileChanged != socketPlug.updatesAccount
        || mutation.accountSoid != socketPlug.after.family4RootSoid
        || socketPlug.accountDefinitionId == 0
        || !state::preview_socket_plug(mutation, account, unlocks)
        || mutation.characterIndex >= account.characterCount
        || account.primarySoid != mutation.accountSoid
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("socket_plug_mutation");
    }

    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.itemInstanceObjectId != socketPlug.itemInstanceDefinitionId) {
        return report_failure("socket_plug_selection");
    }

    family4_datagen::loadout::ResolvedInstances changed{};
    for (std::size_t index = 0; index < selected.loadout.itemCount; ++index) {
        const family4_datagen::loadout::ResolvedItem& item = selected.loadout.items[index];
        if (item.instance.instanceSoid != mutation.targetInstanceSoid) {
            continue;
        }
        if (changed.itemCount != 0
            || item.instance.baseDefinitionIndex != mutation.targetDefinitionIndex
            || mutation.socketLane >= item.instance.ordinarySockets.plugs.size()
            || item.instance.ordinarySockets.state
                   != family4_datagen::instance::OrdinarySocketBlockState::present
            || !item.instance.ordinarySockets.plugs[mutation.socketLane].has_value()
            || *item.instance.ordinarySockets.plugs[mutation.socketLane]
                   != mutation.plugDefinitionIndex) {
            return report_failure("socket_plug_item_shape");
        }
        changed.items[0] = {item.equipmentSlot, item.instance};
        changed.itemCount = 1;
    }
    if (changed.itemCount != 1) {
        return report_failure("socket_plug_item_missing");
    }
    // The target is one of the loadout's items, so the edited ones fit beside it.
    for (std::size_t index = 0; index < selected.loadout.itemCount; ++index) {
        const family4_datagen::loadout::ResolvedItem& item = selected.loadout.items[index];
        if (!item.equipped && item.instance.instanceSoid != mutation.targetInstanceSoid
            && state::item_values_edited(
                mutation.beforeCharacter, mutation.afterCharacter, item.instance.instanceSoid)) {
            changed.items[changed.itemCount++] = {item.equipmentSlot, item.instance};
        }
    }
    // The client reloads held items' content only when a character row changes, which a new
    // serial marks: the oven's after a bake, and each bounty the bake advanced.
    const bool editsCharacter = mutation.updates_character();

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    const std::size_t requiredRawSize = (std::max)(
        {family4_datagen::instance::layout::kObjectSize,
         socketPlug.updatesAccount ? family4_datagen::account::layout::kObjectSize : 0,
         editsCharacter ? family4_datagen::character::layout::kObjectSize : 0});
    if (requiredRawSize > rawStorage.size()) {
        return report_failure("socket_plug_item_storage");
    }
    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::instance::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t itemCursor = 0;
    if (!append_items(scratch,
                      rawStorage,
                      socketPlug.itemInstanceDefinitionId,
                      changed,
                      0,
                      staged,
                      itemCursor,
                      compressedExtent)
        || itemCursor != changed.itemCount) {
        clear_after(scratch, reservation);
        return report_failure("socket_plug_item_object");
    }

    std::size_t objectCount = itemCursor;
    if (socketPlug.updatesAccount) {
        const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
        family4_datagen::account::StateFields accountFields{};
        if (!read_account_state_fields(account, accountFields)
            || !family4_datagen::account::encode(account, accountFields, accountBytes, unlocks)) {
            clear_after(scratch, reservation);
            return report_failure("socket_plug_account_object");
        }
        const char* const ringFailure = write_exchange_changes(
            *reinterpret_cast<family4_datagen::account::layout::Object*>(accountBytes.data()),
            std::span(mutation.changes).first(mutation.changeCount));
        if (ringFailure != nullptr) {
            clear_after(scratch, reservation);
            return report_failure(ringFailure);
        }
        if (objectCount == staged.objects.size()
            || !append_object(scratch,
                              accountBytes,
                              socketPlug.accountDefinitionId,
                              socketPlug.accountSoid,
                              staged.objects[objectCount],
                              compressedExtent)) {
            clear_after(scratch, reservation);
            return report_failure("socket_plug_account_object");
        }
        staged.rawClearSize =
            (std::max)(staged.rawClearSize,
                       reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
        ++objectCount;
    }
    if (editsCharacter) {
        const auto characterBytes =
            rawStorage.first(family4_datagen::character::layout::kObjectSize);
        if (objectCount == staged.objects.size()
            || !family4_datagen::character::encode(account.characters[mutation.characterIndex],
                                                   selected.loadout,
                                                   selected.lightEvaluation,
                                                   characterBytes,
                                                   unlocks)
            || !append_object(scratch,
                              characterBytes,
                              selected.characterObjectId,
                              socketPlug.characterSoid,
                              staged.objects[objectCount],
                              compressedExtent)) {
            clear_after(scratch, reservation);
            return report_failure("socket_plug_character_object");
        }
        staged.rawClearSize = (std::max)(
            staged.rawClearSize,
            reservation.rawWriteOffset + family4_datagen::character::layout::kObjectSize);
        ++objectCount;
    }

    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        socketPlug.after.family4RootSoid,
        socketPlug.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("socket_plug_commit");
    }

    return true;
}

/** Builds the Family-4 subclass item-instance upsert for one prepared node selection. */
bool prepare_subclass_selection(Scratch& scratch,
                                const queuez::SubclassSelection& selection,
                                const state::PendingSubclassSelection& mutation,
                                Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("subclass_select_reservation");
    }

    state::AccountState account{};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.subclassInstanceSoid == 0 || mutation.accountSoid != selection.accountSoid
        || mutation.characterSoid != selection.characterSoid
        || mutation.subclassInstanceSoid != selection.subclassInstanceSoid
        || mutation.accountSoid != selection.after.family4RootSoid
        || selection.itemInstanceDefinitionId == 0
        || !state::preview_subclass_selection(mutation, account)
        || mutation.characterIndex >= account.characterCount
        || account.primarySoid != mutation.accountSoid
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("subclass_select_mutation");
    }

    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.itemInstanceObjectId != selection.itemInstanceDefinitionId) {
        return report_failure("subclass_select_selection");
    }

    family4_datagen::loadout::ResolvedInstances changed{};
    for (std::size_t index = 0; index < selected.loadout.itemCount; ++index) {
        const family4_datagen::loadout::ResolvedItem& item = selected.loadout.items[index];
        if (item.instance.instanceSoid != mutation.subclassInstanceSoid) {
            continue;
        }
        if (changed.itemCount != 0 || !item.equipped
            || item.instance.baseDefinitionIndex != mutation.subclassDefinitionIndex
            || item.instance.socketEntryListIndex != mutation.socketEntryListIndex
            || !item.instance.socketEntryContentsResolved
            || mutation.requestedEntry >= item.instance.socketEntryCount) {
            return report_failure("subclass_select_item_shape");
        }
        changed.items[0] = {item.equipmentSlot, item.instance};
        changed.itemCount = 1;
    }
    if (changed.itemCount != 1) {
        return report_failure("subclass_select_item_missing");
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::instance::layout::kObjectSize > rawStorage.size()) {
        return report_failure("subclass_select_item_storage");
    }
    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::instance::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t itemCursor = 0;
    if (!append_items(scratch,
                      rawStorage,
                      selection.itemInstanceDefinitionId,
                      changed,
                      0,
                      staged,
                      itemCursor,
                      compressedExtent)
        || itemCursor != 1) {
        clear_after(scratch, reservation);
        return report_failure("subclass_select_item_object");
    }

    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        selection.after.family4RootSoid,
        selection.after.family4Version,
        0,
        std::span(staged.objects).first(1),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("subclass_select_commit");
    }

    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
