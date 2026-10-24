#include <limits>
#include <memory>
#include <new>

#include "../../middleware/datagen/family4/loadout/loadout_item_resolver.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "bucket_admission.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace authored_inventory = account::inventory;
namespace family4_loadout = middleware::datagen::family4::loadout;
namespace inventory_buckets = build_data::inventory::buckets;
using namespace runtime::detail;

/** The first matching installed transfer rule controls a vendor claim. */
[[nodiscard]] static bool accepted_claim_rule(std::uint16_t vendorIndex,
                                              std::uint8_t sourceBucket) noexcept {
    std::uint8_t destination = 0;
    return sourceBucket == inventory_buckets::kLostItemsBucketId
           && build_data::find_vendor_transfer_destination(vendorIndex, sourceBucket, destination)
           && destination == build_data::vendors::kAuthoredDestination;
}

struct ClaimScratch {
    AccountState after{};
    family4_loadout::ResolvedLoadout beforeLoadout{};
    family4_loadout::ResolvedLoadout afterLoadout{};
};

/** Prepare a whole-instance move only when its authored bucket has room. */
[[nodiscard]] static bool stage_claim(const AccountState& before,
                                      std::uint16_t vendorIndex,
                                      std::uint8_t sourceBucket,
                                      std::uint64_t instanceSoid,
                                      std::uint16_t definitionIndex,
                                      PendingPostmasterClaim& mutation) noexcept {
    const auto characterIndex = selected_character_index(before);
    if (!account::valid(before) || !valid_profile_inventory(before)
        || characterIndex >= before.characterCount || instanceSoid == 0
        || !accepted_claim_rule(vendorIndex, sourceBucket)) {
        return false;
    }
    const auto& character = before.characters[characterIndex];
    if (character.nextInventorySerial
        >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    std::size_t index = 0;
    while (index < character.inventory.count
           && character.inventory.values[index].instanceSoid != instanceSoid) {
        ++index;
    }
    if (index == character.inventory.count) {
        return false;
    }
    const auto& source = character.inventory.values[index];
    build_data::items::Definition definition{};
    inventory_buckets::Descriptor destination{};
    inventory_buckets::Descriptor postmaster{};
    if (source.placement != authored_inventory::ItemPlacement::lostItems
        || !lost_items_candidate(source, definition, destination)
        || definition.definitionIndex != definitionIndex
        || !family4_loadout::resolve_lost_items_bucket(postmaster)) {
        return false;
    }

    const std::unique_ptr<ClaimScratch> scratch(new (std::nothrow) ClaimScratch{});
    if (!scratch || !family4_loadout::resolve(before, characterIndex, scratch->beforeLoadout)) {
        return false;
    }
    ResolvedPosition sourcePosition{};
    if (!find_resolved_position(scratch->beforeLoadout, instanceSoid, sourcePosition)
        || sourcePosition.equipped || sourcePosition.inventoryRow < postmaster.firstSlot
        || sourcePosition.inventoryRow >= postmaster.firstSlot + postmaster.slotCount) {
        return false;
    }
    scratch->after = before;
    auto& afterCharacter = scratch->after.characters[characterIndex];
    auto& claimed = afterCharacter.inventory.values[index];
    claimed.placement = authored_inventory::ItemPlacement::inventory;
    claimed.mutationSerial = static_cast<std::int32_t>(afterCharacter.nextInventorySerial++);
    ResolvedPosition destinationPosition{};
    if (!account::valid(scratch->after)
        || !family4_loadout::resolve(scratch->after, characterIndex, scratch->afterLoadout)
        || !find_resolved_position(scratch->afterLoadout, instanceSoid, destinationPosition)
        || destinationPosition.equipped || destinationPosition.inventoryRow < destination.firstSlot
        || destinationPosition.inventoryRow >= destination.firstSlot + destination.slotCount) {
        return false;
    }

    mutation.beforeCharacter = character;
    mutation.afterCharacter = afterCharacter;
    mutation.accountSoid = before.primarySoid;
    mutation.characterSoid = character.soid;
    mutation.instanceSoid = instanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.inventoryIndex = index;
    mutation.expectedNextInventorySerial = character.nextInventorySerial;
    mutation.quantity = 1;
    mutation.vendorIndex = vendorIndex;
    mutation.definitionIndex = definitionIndex;
    mutation.sourceBucket = sourceBucket;
    mutation.beforeInventoryRow = sourcePosition.inventoryRow;
    mutation.afterInventoryRow = destinationPosition.inventoryRow;
    mutation.prepared = true;
    return true;
}

/** Rebuild the claim under the transaction lock against the saved before-image. */
[[nodiscard]] static bool materialize_claim(const AccountState& current,
                                            const PendingPostmasterClaim& mutation,
                                            AccountState& after) noexcept {
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.quantity != 1
        || current.primarySoid != mutation.accountSoid
        || mutation.characterIndex >= current.characterCount
        || current.characters[mutation.characterIndex].soid != mutation.characterSoid
        || current.characters[mutation.characterIndex].nextInventorySerial
               != mutation.expectedNextInventorySerial
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)) {
        return false;
    }
    const std::unique_ptr<PendingPostmasterClaim> canonical(new (std::nothrow)
                                                                PendingPostmasterClaim{});
    if (!canonical
        || !stage_claim(current,
                        mutation.vendorIndex,
                        mutation.sourceBucket,
                        mutation.instanceSoid,
                        mutation.definitionIndex,
                        *canonical)
        || canonical->characterIndex != mutation.characterIndex
        || canonical->inventoryIndex != mutation.inventoryIndex
        || canonical->beforeInventoryRow != mutation.beforeInventoryRow
        || canonical->afterInventoryRow != mutation.afterInventoryRow
        || !same_character(canonical->afterCharacter, mutation.afterCharacter)) {
        return false;
    }
    after = current;
    after.characters[mutation.characterIndex] = canonical->afterCharacter;
    return true;
}

bool prepare_postmaster_claim(std::uint16_t vendorIndex,
                              std::uint8_t sourceBucket,
                              std::uint64_t instanceSoid,
                              std::uint16_t definitionIndex,
                              std::int32_t quantity,
                              PendingPostmasterClaim& mutation) noexcept {
    std::destroy_at(&mutation);
    std::construct_at(&mutation);
    if (quantity != 1) {
        return false;
    }
    const std::unique_ptr<AccountState> before(new (std::nothrow) AccountState{});
    return before && investment::store::read_account(*before)
           && stage_claim(
               *before, vendorIndex, sourceBucket, instanceSoid, definitionIndex, mutation);
}

bool preview_postmaster_claim(const PendingPostmasterClaim& mutation,
                              AccountState& after) noexcept {
    std::destroy_at(&after);
    std::construct_at(&after);
    const std::unique_ptr<AccountState> before(new (std::nothrow) AccountState{});
    return before && investment::store::read_account(*before)
           && materialize_claim(*before, mutation, after);
}

bool commit_postmaster_claim(PendingPostmasterClaim& mutation) noexcept {
    const runtime::detail::PendingConsumption consume{mutation};
    investment::store::Transaction transaction;
    const std::unique_ptr<AccountState> before(new (std::nothrow) AccountState{});
    const std::unique_ptr<AccountState> after(new (std::nothrow) AccountState{});
    return transaction.ready() && before && after && investment::store::read_account(*before)
           && materialize_claim(*before, mutation, *after)
           && investment::store::write_account(*after) && transaction.commit();
}

} // namespace sunrise::state
