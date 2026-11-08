/** The default "Emotes" collection item every character carries in its emote slot. */
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;

namespace {

/**
 * Emote definitions seeded into the collection item's wheel lanes.
 * All four are universal Common emotes, so no class or race can reject a seeded lane.
 */
constexpr std::uint32_t kYesEmoteDefinitionHash = 3184938442U;
constexpr std::uint32_t kNopeEmoteDefinitionHash = 48790291U;
constexpr std::uint32_t kCasualSitEmoteDefinitionHash = 383973261U;
constexpr std::uint32_t kCheerEmoteDefinitionHash = 2834933816U;

/** Lane order is the client's own wheel layout. */
constexpr std::array<std::uint32_t, authored_inventory::kEmoteCollectionSocketLaneCount>
    kEmoteCollectionDefaultPlugHashes{
        kCheerEmoteDefinitionHash,     // lane 0 -- top
        kCasualSitEmoteDefinitionHash, // lane 1 -- bottom
        kYesEmoteDefinitionHash,       // lane 2 -- left
        kNopeEmoteDefinitionHash,      // lane 3 -- right
    };

/**
 * Resolves and cross-checks the "Emotes" collection item's own configured content.
 * @param definition Receives the matching native item-definition row.
 * @return True only when both rows agree, declare no native equipment slot, and carry exactly the
 *         expected 4 ordinary socket lanes.
 */
[[nodiscard]] bool
resolve_emote_collection_definition(build_data::items::Definition& definition) noexcept {
    item_details::Definition detail{};
    return build_data::find_item_definition_hash(authored_inventory::kEmoteCollectionDefinitionHash,
                                                 definition)
           && definition.definitionHash == authored_inventory::kEmoteCollectionDefinitionHash
           && build_data::find_configured_item_detail(definition.definitionIndex, detail)
           && detail.definitionIndex == definition.definitionIndex
           && detail.definitionHash == authored_inventory::kEmoteCollectionDefinitionHash
           && detail.bucketId == definition.bucketId && !detail.equipmentSlot.has_value()
           && detail.ordinarySocketState == item_details::OrdinarySocketState::present
           && detail.ordinarySocketCount == authored_inventory::kEmoteCollectionSocketLaneCount;
}

/** @return True when the plug is installed and allowed in that lane of the collection item. */
[[nodiscard]] bool plug_allowed(std::uint16_t collectionDefinitionIndex,
                                std::size_t lane,
                                std::uint32_t plugHash) noexcept {
    build_data::items::Definition plugDefinition{};
    return build_data::find_item_definition_hash(plugHash, plugDefinition)
           && build_data::is_socket_plug_allowed(collectionDefinitionIndex,
                                                 static_cast<std::uint8_t>(lane),
                                                 plugDefinition.definitionIndex);
}

/** Checks every seeded plug is installed and allowed in its lane before an item can carry it. */
[[nodiscard]] bool default_plugs_valid(std::uint16_t collectionDefinitionIndex) noexcept {
    for (std::size_t lane = 0; lane < kEmoteCollectionDefaultPlugHashes.size(); ++lane) {
        if (!plug_allowed(
                collectionDefinitionIndex, lane, kEmoteCollectionDefaultPlugHashes[lane])) {
            return false;
        }
    }
    return true;
}

/** Checks an equipped collection item's plugs so a stale set is repaired, not trusted. */
[[nodiscard]] bool socket_state_sound(const authored_inventory::Item& item,
                                      std::uint16_t collectionDefinitionIndex) noexcept {
    if (item.sockets.policy != authored_inventory::SocketPolicy::authored
        || item.sockets.plugCount != authored_inventory::kEmoteCollectionSocketLaneCount) {
        return false;
    }
    for (std::size_t lane = 0; lane < authored_inventory::kEmoteCollectionSocketLaneCount; ++lane) {
        const std::optional<std::uint32_t>& plugHash = item.sockets.plugs[lane];
        if (!plugHash.has_value() || !plug_allowed(collectionDefinitionIndex, lane, *plugHash)) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * Equips each character with the "Emotes" collection item in the emote slot.
 * Its content declares no native slot, so every resolver reaches it through
 * resolve_native_equipment_slot. Its 4 lanes have no native default and are seeded here.
 */
EmoteCollectionOutcome ensure_character_emote_collection() noexcept {
    // The wheel occupies the authored emote equipment slot.
    constexpr std::size_t kEmoteCollectionSlot =
        static_cast<std::size_t>(authored_inventory::EquipmentSlot::emote);

    // Nothing can be concluded before these domains publish, so this is a retry, not a verdict.
    if (!build_data::item_definitions_ready() || !build_data::configured_item_details_ready()
        || !build_data::socket_plug_rules_ready()) {
        return EmoteCollectionOutcome::notReady;
    }
    // Published and still unresolved means the build cannot carry the wheel; a retry never helps.
    build_data::items::Definition collectionDefinition{};
    if (!resolve_emote_collection_definition(collectionDefinition)
        || !default_plugs_valid(collectionDefinition.definitionIndex)) {
        return EmoteCollectionOutcome::unsupported;
    }

    investment::store::g_mutex.lock();
    AccountState candidate = investment::store::account();
    if (!account::valid(candidate)) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::notReady;
    }
    bool changed = false;
    bool failed = false;
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount && !failed;
         ++characterIndex) {
        CharacterState& character = candidate.characters[characterIndex];
        auto& collectionSlot = character.equipment.slots[kEmoteCollectionSlot];
        const bool present =
            collectionSlot.has_value()
            && collectionSlot->definitionHash == authored_inventory::kEmoteCollectionDefinitionHash;
        if (present && socket_state_sound(*collectionSlot, collectionDefinition.definitionIndex)) {
            continue;
        }
        if (character.nextInventorySerial
            >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
            failed = true;
            break;
        }
        // A repair owns the definition, the sockets and the serial. Every other field, the
        // item-state flags above all, belongs to the player and survives.
        authored_inventory::Item granted = present ? *collectionSlot : authored_inventory::Item{};
        if (!present) {
            std::uint64_t instanceSoid = 0;
            if (!next_item_instance_soid(candidate, instanceSoid)) {
                failed = true;
                break;
            }
            granted.instanceSoid = instanceSoid;
            granted.level = 0;
            granted.quantity = 1;
        }
        granted.definitionHash = authored_inventory::kEmoteCollectionDefinitionHash;
        granted.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
        // Replaced whole: lanes past the used prefix must be empty for the block to validate.
        granted.sockets = authored_inventory::Sockets{};
        granted.sockets.policy = authored_inventory::SocketPolicy::authored;
        granted.sockets.plugCount = kEmoteCollectionDefaultPlugHashes.size();
        for (std::size_t lane = 0; lane < kEmoteCollectionDefaultPlugHashes.size(); ++lane) {
            granted.sockets.plugs[lane] = kEmoteCollectionDefaultPlugHashes[lane];
        }
        collectionSlot = granted;
        changed = true;
    }
    if (failed) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::failed;
    }
    if (!changed) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::ready;
    }
    if (!account::valid(candidate)) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::failed;
    }
    if (!investment::store::write_account(candidate)) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::failed;
    }
    investment::store::g_mutex.unlock();
    return EmoteCollectionOutcome::ready;
}

} // namespace sunrise::state
