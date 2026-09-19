#include <array>

#include "../../../../state/account/account_state.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::abilities;

/** The authored equipment slot that holds the subclass. */
constexpr std::size_t kSubclassSlot =
    static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass);

/**
 * Finds the socket entry list that carries one character's subclass abilities.
 * @param character Authored character.
 * @param socketEntryListIndex Receives the subclass's socket-entry-list index.
 * @return True when the character equips a subclass whose detail is published.
 */
[[nodiscard]] bool subclass_list(const state::CharacterState& character,
                                 std::uint16_t& socketEntryListIndex,
                                 const char*& reason) noexcept {
    const auto& slot = character.equipment.slots[kSubclassSlot];
    state::build_data::items::Definition item{};
    state::build_data::items::details::Definition detail{};
    reason = "subclass_slot";
    if (!slot.has_value()) {
        return false;
    }
    reason = "subclass_item";
    if (!state::build_data::find_item_definition_hash(slot->definitionHash, item)) {
        return false;
    }
    reason = "subclass_detail";
    if (!state::build_data::find_configured_item_detail(item.definitionIndex, detail)) {
        return false;
    }
    socketEntryListIndex = detail.socketEntryListIndex;
    return true;
}

/**
 * @param rows Rows built so far.
 * @param row Candidate whose key is tested against them.
 * @return True when the candidate's key is already held.
 */
[[nodiscard]] bool held(std::span<const domain::Definition> rows,
                        const domain::Definition& row) noexcept {
    for (const domain::Definition& existing : rows) {
        if (existing.socketEntryListIndex == row.socketEntryListIndex
            && existing.selection == row.selection) {
            return true;
        }
    }
    return false;
}

/** @param rows Rows built so far. @return True when this list already has a resolved row. */
[[nodiscard]] bool held(std::span<const state::build_data::socket_entry_buckets::Definition> rows,
                        std::uint16_t socketEntryListIndex) noexcept {
    for (const auto& existing : rows) {
        if (existing.socketEntryListIndex == socketEntryListIndex) {
            return true;
        }
    }
    return false;
}

/** @param item Authored subclass item. @return Its 5 selected socket entries. */
[[nodiscard]] domain::Selection selection_of(const state::account::inventory::Item& item) noexcept {
    return {item.movementAbilityEntry,
            item.grenadeAbilityEntry,
            item.superAbilityEntry,
            item.meleeAbilityEntry,
            item.classAbilityEntry};
}

} // namespace

/** Builds one ability bucket row per distinct subclass and ability selection in use. */
bool build_character_abilities(
    const reader::Source& source,
    reader::Scratch& scratch,
    std::span<const std::byte> root,
    std::vector<std::byte>& table,
    std::vector<std::byte>& definition,
    std::vector<std::byte>& blob,
    std::span<state::build_data::abilities::Definition> output,
    std::size_t& count,
    std::span<state::build_data::socket_entry_buckets::Definition> entryBucketOutput,
    std::size_t& entryBucketCount) noexcept {
    count = 0;
    entryBucketCount = 0;
    std::uint32_t tableTag = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kSocketEntryListTableSlot, tableTag) || tableTag == 0) {
        report_ability_failure("table_slot", 0, root.size(), tableTag);
        return false;
    }
    if (!reader::read_tag(source, scratch, tableTag, table)) {
        report_ability_failure("table_read", 0, tableTag, 0);
        return false;
    }
    if (!tables::find_array_at(
            std::span<const std::byte>{table}, tables::kTableArrayDescriptor, rows)) {
        report_ability_failure("table_array", 0, table.size(), 0);
        return false;
    }
    const state::AccountState account = state::bound_account_snapshot();
    // First-option defaults every shipped subclass starts at. Equipping one resets its picks to
    // these, so every owned subclass publishes a row at this selection. The row must exist
    // synchronously: the equip response is built inline with the commit.
    const domain::Selection defaultSelection{state::kDefaultMovementAbilityEntry,
                                             state::kDefaultGrenadeAbilityEntry,
                                             state::kDefaultSuperAbilityEntry,
                                             state::kDefaultMeleeAbilityEntry,
                                             state::kDefaultClassAbilityEntry};
    // Builds and stores one row, skipping a key already held. Best-effort: a failure here still
    // lets the other rows in this pass publish.
    const auto publish = [&](std::size_t character,
                             std::uint16_t socketEntryListIndex,
                             const domain::Selection& selection) noexcept {
        if (count >= output.size()) {
            return;
        }
        domain::Definition row{};
        row.socketEntryListIndex = socketEntryListIndex;
        row.selection = selection;
        if (held(output.first(count), row)) {
            return;
        }
        tables::IndexRow indexRow{};
        if (!tables::index_row(
                std::span<const std::byte>{table}, rows, socketEntryListIndex, indexRow)
            || indexRow.targetTag == 0) {
            report_ability_failure("index_row", character, socketEntryListIndex, rows.count);
            return;
        }
        if (!reader::read_tag(source, scratch, indexRow.targetTag, definition)) {
            report_ability_failure(
                "definition_read", character, socketEntryListIndex, indexRow.targetTag);
            return;
        }
        // A bundled group can freely mix which ability slot each of its members fills (an
        // Attunement's melee and super swap can sit in either position), so this is resolved once
        // per list here, independent of any selection, rather than assumed from table position.
        if (entryBucketCount < entryBucketOutput.size()
            && !held(entryBucketOutput.first(entryBucketCount), socketEntryListIndex)) {
            state::build_data::socket_entry_buckets::Definition entryBuckets{};
            entryBuckets.socketEntryListIndex = socketEntryListIndex;
            if (resolve_entry_buckets(source,
                                      scratch,
                                      std::span<const std::byte>{definition},
                                      blob,
                                      entryBuckets.buckets)) {
                entryBucketOutput[entryBucketCount++] = entryBuckets;
            }
        }
        if (!build_ability_buckets(
                source, scratch, std::span<const std::byte>{definition}, blob, selection, row)) {
            const std::size_t packedSelection =
                selection.movementEntry | (selection.grenadeEntry << 8U)
                | (selection.superEntry << 16U) | (selection.meleeEntry << 24U);
            report_ability_failure(
                "bucket_build", character, socketEntryListIndex, packedSelection);
            return;
        }
        output[count++] = row;
    };
    for (std::size_t character = 0; character < account.characterCount && count < output.size();
         ++character) {
        std::uint16_t equippedSocketEntryListIndex = 0;
        const char* subclassReason = "subclass";
        if (!subclass_list(
                account.characters[character], equippedSocketEntryListIndex, subclassReason)) {
            const auto& subclass = account.characters[character].equipment.slots[kSubclassSlot];
            report_ability_failure(
                subclassReason, character, subclass.has_value() ? subclass->definitionHash : 0, 0);
            continue;
        }

        const auto& equippedSlot = account.characters[character].equipment.slots[kSubclassSlot];
        state::build_data::items::Definition equippedItem{};
        if (!state::build_data::find_item_definition_hash(equippedSlot->definitionHash,
                                                          equippedItem)) {
            continue;
        }

        // Every subclass the character owns publishes a row, not just the equipped one, so a
        // later equip swap lands on an already-built row instead of racing the next refresh
        // slice. When the group cannot be resolved, the equipped one still publishes.
        std::array<std::uint16_t, state::build_data::kSubclassGroupSize> group{};
        std::array<std::uint16_t, state::build_data::kSubclassGroupSize> members{};
        std::size_t memberCount = 1;
        members[0] = equippedItem.definitionIndex;
        if (state::build_data::find_subclass_group(equippedItem.definitionIndex, group)) {
            members = group;
            memberCount = group.size();
        }

        for (std::size_t member = 0; member < memberCount && count < output.size(); ++member) {
            const std::uint16_t memberDefinitionIndex = members[member];
            state::build_data::items::details::Definition memberDetail{};
            if (!state::build_data::find_configured_item_detail(memberDefinitionIndex,
                                                                memberDetail)) {
                continue;
            }
            publish(character, memberDetail.socketEntryListIndex, defaultSelection);
            // Each owned subclass remembers its own picks, so every member is checked for a
            // non-default selection to publish on top of its default row. A fresh boot needs the
            // actual selection for subclasses configured before this boot.
            const domain::Selection* memberSelection = nullptr;
            domain::Selection resolvedSelection{};
            if (memberDefinitionIndex == equippedItem.definitionIndex) {
                resolvedSelection = selection_of(*equippedSlot);
                memberSelection = &resolvedSelection;
            } else {
                state::build_data::items::Definition memberItemDefinition{};
                if (state::build_data::find_item_definition_index(memberDefinitionIndex,
                                                                  memberItemDefinition)) {
                    const auto& inventory = account.characters[character].inventory;
                    for (std::size_t itemIndex = 0; itemIndex < inventory.count; ++itemIndex) {
                        if (inventory.values[itemIndex].definitionHash
                            == memberItemDefinition.definitionHash) {
                            resolvedSelection = selection_of(inventory.values[itemIndex]);
                            memberSelection = &resolvedSelection;
                            break;
                        }
                    }
                }
            }
            if (memberSelection != nullptr && !(*memberSelection == defaultSelection)) {
                publish(character, memberDetail.socketEntryListIndex, *memberSelection);
            }
        }
    }
    if (count == 0) {
        report_ability_failure("empty", account.characterCount, rows.count, output.size());
    }
    return true;
}

} // namespace sunrise::client::content::items::packages
