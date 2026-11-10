#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include "../account/inventory/occupied_bucket.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "bucket_admission.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

using namespace runtime::detail;

namespace {

namespace inventory_buckets = build_data::inventory::buckets;
namespace item_details = build_data::items::details;

/** Finds the installed item a quantity edit names, after checking the quantity is not negative. */
[[nodiscard]] bool find_edited_item(const QuantityEdit& edit,
                                    build_data::items::Definition& definition,
                                    const char*& reason) noexcept {
    reason = "quantity";
    if (edit.quantity < 0) {
        return false;
    }
    reason = "item_definition";
    return build_data::find_item_definition_hash(edit.definitionHash, definition);
}

/**
 * Checks that the scope names the list holding the bucket a row occupies. A recovery placement
 * puts the row in its item's recovery bucket, whose list can differ from the authored bucket's.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool scope_matches(const QuantityEdit& edit,
                                 const build_data::items::Definition& definition,
                                 account::inventory::ItemPlacement placement,
                                 const char*& reason) noexcept {
    reason = "item_definition";
    inventory_buckets::Descriptor bucket{};
    if (!account::inventory::find_occupied_bucket(definition, placement, bucket)) {
        return false;
    }
    reason = "inventory_scope";
    switch (edit.scope) {
    case GrantedRowArray::profile:
        return bucket.arraySelector == inventory_buckets::ArraySelector::profile;
    case GrantedRowArray::characterItem:
        return bucket.arraySelector == inventory_buckets::ArraySelector::character
               && edit.instanceSoid != 0;
    case GrantedRowArray::characterStack:
        return bucket.arraySelector == inventory_buckets::ArraySelector::character
               && edit.instanceSoid == 0;
    }
    return false;
}

/** A nonzero replacement must be a stackable item within its native stack limit. */
[[nodiscard]] bool replacement_fits(const build_data::items::Definition& definition,
                                    const QuantityEdit& edit,
                                    const char*& reason) noexcept {
    reason = "item_detail";
    item_details::Definition detail{};
    if (!build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId) {
        return false;
    }
    reason = "quantity";
    return detail.instancedDefinitionState == item_details::InstancedDefinitionState::stackable
           && edit.quantity <= detail.maxStackSize;
}

/**
 * Replaces the expected row's quantity and serial, or erases the row when the quantity is zero.
 * The row must occupy a bucket of the list the scope names.
 * @param reason Receives inventory_row when no row carries the expected identity and quantity.
 * @param erase Removes the row at an index of this array.
 */
template <typename Row, std::size_t Capacity, typename Erase>
[[nodiscard]] bool edit_row(std::array<Row, Capacity>& rows,
                            std::size_t count,
                            const build_data::items::Definition& definition,
                            const QuantityEdit& edit,
                            std::int32_t serial,
                            const char*& reason,
                            Erase erase) noexcept {
    for (std::size_t index = 0; index < count && index < Capacity; ++index) {
        Row& row = rows[index];
        if (row.definitionHash != edit.definitionHash || row.mutationSerial != edit.mutationSerial
            || row.quantity != edit.expectedQuantity) {
            continue;
        }
        // Character stacks carry no instance.
        if constexpr (requires { row.instanceSoid; }) {
            if (row.instanceSoid != edit.instanceSoid) {
                continue;
            }
        }
        // Saved profile rows record no placement, so they occupy their authored bucket.
        auto placement = account::inventory::ItemPlacement::authored;
        if constexpr (requires { row.placement; }) {
            placement = row.placement;
        }
        if (!scope_matches(edit, definition, placement, reason)) {
            return false;
        }
        reason = "inventory_row";
        if (edit.quantity == 0) {
            return erase(index);
        }
        row.quantity = edit.quantity;
        row.mutationSerial = serial;
        return true;
    }
    reason = "inventory_row";
    return false;
}

/** Applies the edit to the list its scope names. The new serial comes from the list's owner. */
[[nodiscard]] bool apply_edit(AccountState& saved,
                              CharacterState& character,
                              const build_data::items::Definition& definition,
                              const QuantityEdit& edit,
                              const char*& reason) noexcept {
    reason = "serial_exhausted";
    std::int32_t serial = 0;
    if (edit.quantity != 0
        && !(edit.scope == GrantedRowArray::profile ? next_profile_serial(saved, serial)
                                                    : next_character_serial(character, serial))) {
        return false;
    }
    switch (edit.scope) {
    case GrantedRowArray::profile:
        return edit_row(
            saved.profileItems,
            saved.profileItemCount,
            definition,
            edit,
            serial,
            reason,
            [&saved](std::size_t index) noexcept { return erase_profile_row(saved, index); });
    case GrantedRowArray::characterItem:
        return edit_row(character.inventory.values,
                        character.inventory.count,
                        definition,
                        edit,
                        serial,
                        reason,
                        [&character](std::size_t index) noexcept {
                            return erase_character_row(character, index);
                        });
    case GrantedRowArray::characterStack:
        return edit_row(character.stacks.values,
                        character.stacks.count,
                        definition,
                        edit,
                        serial,
                        reason,
                        [&character](std::size_t index) noexcept {
                            return erase_character_row(character, kStackRowBase + index);
                        });
    }
    reason = "inventory_row";
    return false;
}

} // namespace

/** Checks the quantity and the item, which needs only the installed item catalog. */
bool check_quantity_edit(const QuantityEdit& edit, const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    build_data::items::Definition definition{};
    if (!find_edited_item(edit, definition, reason)) {
        return false;
    }
    reason = nullptr;
    return true;
}

/** Replaces one expected row's quantity and saves the whole account in one savepoint. */
bool set_quantity(const QuantityEdit& edit,
                  AccountEditChange& change,
                  const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    change = {};
    build_data::items::Definition definition{};
    if (!find_edited_item(edit, definition, reason)) {
        return false;
    }
    if (edit.quantity == 0) {
        reason = "unsupported_removal";
        if (edit.instanceSoid != 0) {
            return false;
        }
    } else if (!replacement_fits(definition, edit, reason)) {
        return false;
    }

    reason = "allocation";
    const std::unique_ptr<AccountState> image(new (std::nothrow) AccountState{});
    if (!image) {
        return false;
    }
    AccountState& saved = *image;
    reason = "transaction";
    investment::store::Transaction transaction;
    if (!transaction.ready() || !investment::store::read_account(saved)) {
        return false;
    }
    reason = "account_state";
    if (!account::valid(saved)) {
        return false;
    }
    reason = "character_changed";
    const std::size_t characterIndex = selected_character_index(saved);
    if (characterIndex >= saved.characterCount
        || saved.characters[characterIndex].soid != edit.characterSoid) {
        return false;
    }
    CharacterState& character = saved.characters[characterIndex];
    reason = "equipped";
    CharacterItemLocation location{};
    if (edit.instanceSoid != 0
        && find_character_item_location(character, edit.instanceSoid, location)
        && location.equipped) {
        return false;
    }
    if (!apply_edit(saved, character, definition, edit, reason)) {
        return false;
    }
    reason = "account_state";
    if (!account::valid(saved)) {
        return false;
    }
    reason = "transaction";
    if (!investment::store::write_account(saved) || !transaction.commit()) {
        return false;
    }
    // Profile rows live in the account object, and character rows only in the character object.
    change = {
        saved.primarySoid, character.soid, characterIndex, edit.scope == GrantedRowArray::profile};
    reason = nullptr;
    return true;
}

} // namespace sunrise::state
