#include "bucket_admission.h"

#include <algorithm>
#include <memory>
#include <new>

#include "../../middleware/datagen/family4/loadout/loadout_item_resolver.h"
#include "../account/inventory/postmaster_policy.h"
#include "../build_data/runtime.h"

namespace sunrise::state::runtime::detail {
namespace authored_inventory = account::inventory;
namespace family4_loadout = middleware::datagen::family4::loadout;
namespace inventory_buckets = build_data::inventory::buckets;

/** Placement overrides the authored bucket only for Lost Items. */
[[nodiscard]] static bool
in_bucket(const authored_inventory::Item& item, std::uint8_t bucketId, bool& matches) noexcept {
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_hash(item.definitionHash, definition)) {
        return false;
    }
    const auto placedBucket = item.placement == authored_inventory::ItemPlacement::lostItems
                                  ? inventory_buckets::kLostItemsBucketId
                                  : definition.bucketId;
    matches = placedBucket == bucketId;
    return true;
}

bool character_bucket_admission(const CharacterState& character,
                                const inventory_buckets::Descriptor& bucket,
                                BucketAdmission& occupancy) noexcept {
    occupancy = {};
    if (character.inventory.count > character.inventory.values.size()
        || character.stacks.count > character.stacks.values.size()) {
        return false;
    }
    // Equipped rows count toward capacity but are never offered for eviction.
    for (const auto& equipped : character.equipment.slots) {
        bool matches = false;
        if (equipped && !in_bucket(*equipped, bucket.bucketId, matches)) {
            return false;
        }
        occupancy.count += matches;
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        const auto& item = character.inventory.values[index];
        bool matches = false;
        if (!in_bucket(item, bucket.bucketId, matches)) {
            return false;
        }
        if (matches) {
            occupancy.include(index, item.mutationSerial);
        }
    }
    for (std::size_t index = 0; index < character.stacks.count; ++index) {
        const auto& item = character.stacks.values[index];
        build_data::items::Definition definition{};
        if (!build_data::find_item_definition_hash(item.definitionHash, definition)) {
            return false;
        }
        if (definition.bucketId == bucket.bucketId) {
            occupancy.include(kCharacterStackAdmissionBase + index, item.mutationSerial);
        }
    }
    return true;
}

bool erase_character_bucket_row(CharacterState& character, std::size_t row) noexcept {
    if (row < kCharacterStackAdmissionBase) {
        auto& held = character.inventory;
        if (row >= held.count || held.count > held.values.size()) {
            return false;
        }
        std::move(held.values.begin() + static_cast<std::ptrdiff_t>(row) + 1,
                  held.values.begin() + static_cast<std::ptrdiff_t>(held.count),
                  held.values.begin() + static_cast<std::ptrdiff_t>(row));
        held.values[--held.count] = {};
    } else {
        auto& held = character.stacks;
        row -= kCharacterStackAdmissionBase;
        if (row >= held.count || held.count > held.values.size()) {
            return false;
        }
        std::move(held.values.begin() + static_cast<std::ptrdiff_t>(row) + 1,
                  held.values.begin() + static_cast<std::ptrdiff_t>(held.count),
                  held.values.begin() + static_cast<std::ptrdiff_t>(row));
        held.values[--held.count] = {};
    }
    return true;
}
bool lost_items_candidate(const authored_inventory::Item& item,
                          build_data::items::Definition& definition,
                          inventory_buckets::Descriptor& bucket) noexcept {
    build_data::items::details::Definition detail{};
    return build_data::find_item_definition_hash(item.definitionHash, definition)
           && definition.definitionHash == item.definitionHash
           && build_data::find_configured_item_detail(definition.definitionIndex, detail)
           && detail.definitionIndex == definition.definitionIndex
           && detail.definitionHash == definition.definitionHash
           && detail.bucketId == definition.bucketId
           && authored_inventory::lost_items_supported(item, detail)
           && build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
           && bucket.bucketId == definition.bucketId
           && bucket.arraySelector == inventory_buckets::ArraySelector::character
           && bucket.bucketId != inventory_buckets::kLostItemsBucketId
           && bucket.bucketId != inventory_buckets::kPerkBucketId && bucket.slotCount != 0;
}

bool place_instanced_reward(const AccountState& before,
                            std::size_t characterIndex,
                            authored_inventory::Item& item,
                            CharacterState& after,
                            std::uint64_t& evictedInstanceSoid) noexcept {
    evictedInstanceSoid = 0;
    if (!account::valid(before) || characterIndex >= before.characterCount
        || item.placement != authored_inventory::ItemPlacement::inventory) {
        return false;
    }
    const std::unique_ptr<family4_loadout::Candidate> candidate(new (std::nothrow)
                                                                    family4_loadout::Candidate{});
    if (!candidate
        || !family4_loadout::resolve_item(item,
                                          before.characters[characterIndex],
                                          build_data::item_definition_count(),
                                          build_data::socket_entry_list_count(),
                                          false,
                                          *candidate)) {
        return false;
    }
    const auto& bucket = candidate->bucket;
    const auto& character = before.characters[characterIndex];
    BucketAdmission home;
    if (!character_bucket_admission(character, bucket, home)) {
        return false;
    }
    std::size_t insertion = kCharacterAdmissionCapacity;
    if (home.select(bucket, kCharacterAdmissionCapacity, insertion)) {
        if (insertion == kCharacterAdmissionCapacity) {
            return true;
        }
        if (insertion < kCharacterStackAdmissionBase) {
            evictedInstanceSoid = character.inventory.values[insertion].instanceSoid;
        }
        return erase_character_bucket_row(after, insertion);
    }
    if (home.count != bucket.slotCount
        || (bucket.policyFlags & inventory_buckets::kNoTransferOnEviction) != 0) {
        return false;
    }
    build_data::items::Definition definition{};
    inventory_buckets::Descriptor destination{};
    inventory_buckets::Descriptor postmaster{};
    if (!lost_items_candidate(item, definition, destination)
        || !family4_loadout::resolve_lost_items_bucket(postmaster)) {
        return false;
    }
    BucketAdmission lostItems;
    if (!character_bucket_admission(character, postmaster, lostItems)
        || !lostItems.select(postmaster, kCharacterAdmissionCapacity, insertion)) {
        return false;
    }
    if (insertion != kCharacterAdmissionCapacity) {
        if (insertion >= character.inventory.count) {
            return false;
        }
        evictedInstanceSoid = character.inventory.values[insertion].instanceSoid;
        if (!erase_character_bucket_row(after, insertion)) {
            return false;
        }
    }
    item.placement = authored_inventory::ItemPlacement::lostItems;
    return true;
}
} // namespace sunrise::state::runtime::detail
