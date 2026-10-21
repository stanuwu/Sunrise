#include "loadout_item_resolver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../../../state/account/inventory/postmaster_policy.h"
#include "../../../../state/build_data/runtime.h"
#include "subclass_socket_selection.h"

namespace sunrise::middleware::datagen::family4::loadout {
namespace {

namespace authored_inventory = state::account::inventory;
namespace build_buckets = state::build_data::inventory::buckets;
namespace build_details = state::build_data::items::details;
namespace build_items = state::build_data::items;
namespace build_socket_lists = state::build_data::socket_entry_lists;

/** Native instanced inventory rows always carry exactly one item. */
constexpr std::int32_t kInstancedQuantity = 1;

/**
 * Finds the installed item index for one authored plug hash.
 * @param definitionHash Authored plug definition hash.
 * @param itemDefinitionCount Stable dense item-table row count.
 * @param output Receives the installed plug index.
 * @return True when the hash has one mapping inside the stable table bound.
 */
[[nodiscard]] bool resolve_plug(std::uint32_t definitionHash,
                                std::size_t itemDefinitionCount,
                                std::uint16_t& output) noexcept {
    build_items::Definition definition{};
    if (!state::build_data::find_item_definition_hash(definitionHash, definition)
        || static_cast<std::size_t>(definition.definitionIndex) >= itemDefinitionCount) {
        return false;
    }
    output = definition.definitionIndex;
    return true;
}

/**
 * Resolves native-default or authored ordinary socket lanes.
 * @param authored Authored socket policy and optional plug hashes.
 * @param definition Installed item detail carrying the native socket shape.
 * @param itemDefinitionCount Stable dense item-table row count.
 * @param output Receives the fully resolved ordinary socket block.
 * @return True when policy, lane count, and every present plug resolve.
 */
[[nodiscard]] bool resolve_ordinary_sockets(const authored_inventory::Sockets& authored,
                                            const build_details::Definition& definition,
                                            std::size_t itemDefinitionCount,
                                            instance::OrdinarySockets& output) noexcept {
    output = {};
    if (definition.ordinarySocketState == build_details::OrdinarySocketState::absent) {
        if (authored.policy == authored_inventory::SocketPolicy::authored
            && authored.plugCount != 0) {
            return false;
        }
        output.state = instance::OrdinarySocketBlockState::absent;
        return true;
    }
    if (definition.ordinarySocketState != build_details::OrdinarySocketState::present) {
        return false;
    }
    output.state = instance::OrdinarySocketBlockState::present;

    if (authored.policy == authored_inventory::SocketPolicy::nativeDefaults) {
        for (std::size_t index = 0; index < definition.initialPlugIndices.size(); ++index) {
            const std::uint16_t plugIndex = definition.initialPlugIndices[index];
            if (plugIndex == build_details::kUnavailableItemIndex) {
                continue;
            }
            if (static_cast<std::size_t>(plugIndex) >= itemDefinitionCount) {
                return false;
            }
            output.plugs[index] = plugIndex;
        }
        return true;
    }
    if (authored.policy != authored_inventory::SocketPolicy::authored
        || authored.plugCount != definition.ordinarySocketCount) {
        return false;
    }
    for (std::size_t index = 0; index < authored.plugCount; ++index) {
        const std::optional<std::uint32_t>& definitionHash = authored.plugs[index];
        if (!definitionHash.has_value()) {
            continue;
        }
        std::uint16_t plugIndex = 0;
        if (!resolve_plug(*definitionHash, itemDefinitionCount, plugIndex)) {
            return false;
        }
        output.plugs[index] = plugIndex;
    }
    return true;
}

/**
 * Applies the native instanced-or-stackable quantity policy.
 * @param authored Positive authored quantity.
 * @param definition Installed item detail carrying the stack limit and predicate.
 * @param output Receives a positive row quantity.
 * @return True when the normalized predicate is supported.
 */
[[nodiscard]] bool resolve_quantity(const authored_inventory::Item& authored,
                                    const build_details::Definition& definition,
                                    std::int32_t& output) noexcept {
    switch (definition.instancedDefinitionState) {
    case build_details::InstancedDefinitionState::stackable:
        output = (std::min)(authored.quantity, definition.maxStackSize);
        return output > 0;
    case build_details::InstancedDefinitionState::instanced:
        output = kInstancedQuantity;
        return true;
    default:
        return false;
    }
}

} // namespace

/** Resolves one authored item into native mappings without choosing an inventory row. */
bool resolve_item(const authored_inventory::Item& authored,
                  const state::CharacterState& character,
                  std::size_t itemDefinitionCount,
                  std::size_t socketEntryListCount,
                  bool requireEquipmentSlot,
                  Candidate& output) noexcept {
    if (!authored_inventory::valid(authored) || itemDefinitionCount == 0
        || itemDefinitionCount > build_items::kDefinitionCapacity || socketEntryListCount == 0
        || socketEntryListCount > build_socket_lists::kDefinitionCapacity) {
        return false;
    }

    build_items::Definition itemDefinition{};
    build_details::Definition itemDetail{};
    build_buckets::Descriptor bucket{};
    build_socket_lists::Definition socketList{};
    std::uint8_t nativeEquipmentSlot = 0;
    if (!state::build_data::find_item_definition_hash(authored.definitionHash, itemDefinition)
        || !state::build_data::find_configured_item_detail(itemDefinition.definitionIndex,
                                                           itemDetail)
        || itemDefinition.bucketId != itemDetail.bucketId
        // A pursuit names no equipment slot, so do not require one. An equipped item still must
        // name a slot: it comes out of the equipment array, where the slot identifies it.
        || (!authored_inventory::resolve_native_equipment_slot(
                authored.definitionHash, itemDetail.equipmentSlot, nativeEquipmentSlot)
            && (requireEquipmentSlot || itemDetail.equipmentSlot.has_value()))
        || static_cast<std::size_t>(nativeEquipmentSlot) >= build_details::kEquipmentSlotCount
        || !state::build_data::find_inventory_bucket_descriptor(itemDetail.bucketId, bucket)
        || bucket.arraySelector != build_buckets::ArraySelector::character
        || !state::build_data::find_socket_entry_list(itemDetail.socketEntryListIndex, socketList)
        || static_cast<std::size_t>(itemDefinition.definitionIndex) >= itemDefinitionCount
        || static_cast<std::size_t>(socketList.definitionIndex) >= socketEntryListCount
        || socketList.definitionIndex != itemDetail.socketEntryListIndex) {
        return false;
    }

    if (authored.placement == authored_inventory::ItemPlacement::lostItems
        && (requireEquipmentSlot || !authored_inventory::lost_items_supported(authored, itemDetail)
            || !resolve_lost_items_bucket(bucket))) {
        return false;
    }
    Candidate candidate{};
    candidate.bucket = bucket;
    // Slot zero for a slotless item is safe: the encoder reads `equipmentSlot` only when `equipped`
    // is set, and only items resolved out of the equipment array are ever equipped.
    candidate.item.equipmentSlot = nativeEquipmentSlot;
    candidate.item.mutationSerial = authored.mutationSerial;
    candidate.item.flags = authored.flags;
    candidate.item.seen = authored.seen;
    if (!resolve_quantity(authored, itemDetail, candidate.item.quantity)
        || !resolve_ordinary_sockets(authored.sockets,
                                     itemDetail,
                                     itemDefinitionCount,
                                     candidate.item.instance.ordinarySockets)) {
        return false;
    }
    std::uint32_t completedFlags = candidate.item.flags;
    auto completedPlugs = candidate.item.instance.ordinarySockets.plugs;
    if (state::build_data::complete_exotic_catalyst(
            itemDefinition.definitionIndex, completedFlags, completedPlugs)
        == state::build_data::items::catalysts::ApplyResult::completed) {
        candidate.item.flags = completedFlags;
        candidate.item.instance.ordinarySockets.plugs = completedPlugs;
    }

    candidate.item.instance.instanceSoid = authored.instanceSoid;
    candidate.item.instance.bounds.itemDefinitionCount =
        static_cast<std::uint32_t>(itemDefinitionCount);
    candidate.item.instance.bounds.socketEntryListCount =
        static_cast<std::uint32_t>(socketEntryListCount);
    candidate.item.instance.baseDefinitionIndex = itemDefinition.definitionIndex;
    candidate.item.instance.level = authored.level;
    candidate.item.instance.curveSelector = instance::layout::kInitialLevelCurveX;
    candidate.item.instance.capSelector = instance::layout::kInitialLevelCapRow;
    candidate.item.instance.socketEntryListIndex = socketList.definitionIndex;
    candidate.item.instance.socketEntryCount = socketList.entryCount;
    candidate.item.instance.socketEntryContentsResolved = true;
    resolve_socket_states(socketList,
                          authored,
                          character.characterClass,
                          character.acquiredSubclassAbilityMask,
                          candidate.item.instance.socketEntryStates,
                          candidate.item.instance.socketSelectors);
    output = candidate;
    return true;
}

bool resolve_lost_items_bucket(build_buckets::Descriptor& output) noexcept {
    build_buckets::Descriptor bucket{};
    if (!state::build_data::find_inventory_bucket_descriptor(build_buckets::kLostItemsBucketId,
                                                             bucket)
        || bucket.bucketId != build_buckets::kLostItemsBucketId
        || bucket.arraySelector != build_buckets::ArraySelector::character || bucket.slotCount == 0
        || bucket.equipmentSlot != build_buckets::kUnavailableEquipmentSlot
        || (bucket.policyFlags & build_buckets::kPolicyMask) != build_buckets::kFifo) {
        return false;
    }
    output = bucket;
    return true;
}

} // namespace sunrise::middleware::datagen::family4::loadout
