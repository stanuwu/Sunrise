/** Socket-plug and item-state staging, which both mutate one character-owned item. */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

#include "../../core/logging/log.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state.h"
#include "state_account_transaction_helpers.h"
#include "state_rolled_socket_plugs.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace runtime::detail {

namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;
namespace family4_loadout = middleware::datagen::family4::loadout;

/** Writes one bounded opcode-903 socket-selection transaction checkpoint. */
void report_socket_plug(std::string_view stage,
                        std::string_view result,
                        std::string_view reason,
                        std::uint64_t characterSoid,
                        std::uint64_t targetInstanceSoid,
                        std::uint16_t targetDefinitionIndex,
                        std::uint8_t socketLane,
                        std::uint16_t plugDefinitionIndex,
                        std::uint8_t targetBucketId,
                        std::uint8_t plugBucketId,
                        bool targetEquipped,
                        std::size_t itemIndex) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=socket_plug stage=%.*s result=%.*s reason=%.*s character=0x%llX "
        "instance=0x%llX target_definition=%u target_bucket=%u lane=%u plug_definition=%u "
        "plug_bucket=%u equipped=%u item_index=%zu",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned long long>(characterSoid),
        static_cast<unsigned long long>(targetInstanceSoid),
        static_cast<unsigned>(targetDefinitionIndex),
        static_cast<unsigned>(targetBucketId),
        static_cast<unsigned>(socketLane),
        static_cast<unsigned>(plugDefinitionIndex),
        static_cast<unsigned>(plugBucketId),
        static_cast<unsigned>(targetEquipped),
        itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Materializes native initial plugs as a complete authored socket block. */
[[nodiscard]] bool materialize_native_sockets(const item_details::Definition& detail,
                                              authored_inventory::Sockets& sockets) noexcept {
    sockets = {};
    if (detail.ordinarySocketState != item_details::OrdinarySocketState::present
        || detail.ordinarySocketCount > sockets.plugs.size()) {
        return false;
    }
    sockets.policy = authored_inventory::SocketPolicy::authored;
    sockets.plugCount = detail.ordinarySocketCount;
    for (std::size_t lane = 0; lane < sockets.plugCount; ++lane) {
        const std::uint16_t plugIndex = detail.initialPlugIndices[lane];
        if (plugIndex == item_details::kUnavailableItemIndex) {
            continue;
        }
        build_data::items::Definition plug{};
        if (!build_data::find_item_definition_index(plugIndex, plug)
            || plug.definitionIndex != plugIndex
            || plug.definitionHash == authored_inventory::kNoDefinitionHash) {
            return false;
        }
        sockets.plugs[lane] = plug.definitionHash;
    }
    return authored_inventory::valid(sockets);
}

/** Stages the canonical socket-only after-image over one already validated account snapshot. */
[[nodiscard]] bool stage_socket_plug(const AccountState& snapshot,
                                     std::size_t characterIndex,
                                     std::uint64_t targetInstanceSoid,
                                     std::uint8_t socketLane,
                                     std::uint16_t plugDefinitionIndex,
                                     PendingSocketPlug& mutation,
                                     std::uint32_t pinnedPlugHash) noexcept {
    mutation = {};
    CharacterItemLocation location{};
    build_data::items::Definition targetDefinition{};
    build_data::items::Definition plugDefinition{};
    const auto fail = [&](std::string_view reason) noexcept {
        const std::uint64_t characterSoid =
            characterIndex < snapshot.characterCount ? snapshot.characters[characterIndex].soid : 0;
        report_socket_plug("stage_internal",
                           "fail",
                           reason,
                           characterSoid,
                           targetInstanceSoid,
                           targetDefinition.definitionIndex,
                           socketLane,
                           plugDefinitionIndex,
                           targetDefinition.bucketId,
                           plugDefinition.bucketId,
                           location.equipped,
                           location.index);
        mutation = {};
        return false;
    };
    if (!account::valid(snapshot) || characterIndex >= snapshot.characterCount
        || targetInstanceSoid == 0 || socketLane >= authored_inventory::kPlugCapacity) {
        return fail("request_or_account");
    }
    const CharacterState& before = snapshot.characters[characterIndex];
    if (!before.selected || before.soid == 0) {
        return fail("selected_character");
    }

    family4_loadout::ResolvedLoadout beforeLoadout{};
    if (!find_character_item_location(before, targetInstanceSoid, location)
        || !family4_loadout::resolve(snapshot, characterIndex, beforeLoadout)) {
        return fail("target_or_before_loadout");
    }
    const authored_inventory::Item* target = character_item_at(before, location);
    item_details::Definition detail{};
    if (target == nullptr
        || !build_data::find_item_definition_hash(target->definitionHash, targetDefinition)
        || targetDefinition.definitionHash != target->definitionHash
        || !build_data::find_configured_item_detail(targetDefinition.definitionIndex, detail)
        || detail.definitionIndex != targetDefinition.definitionIndex
        || detail.definitionHash != targetDefinition.definitionHash
        || detail.bucketId != targetDefinition.bucketId
        || detail.ordinarySocketState != item_details::OrdinarySocketState::present
        || socketLane >= detail.ordinarySocketCount
        || detail.ordinarySocketCount > authored_inventory::kPlugCapacity
        || !build_data::find_item_definition_index(plugDefinitionIndex, plugDefinition)
        || plugDefinition.definitionIndex != plugDefinitionIndex
        || plugDefinition.definitionHash == authored_inventory::kNoDefinitionHash
        || !build_data::is_socket_plug_allowed(
            targetDefinition.definitionIndex, socketLane, plugDefinitionIndex)) {
        return fail("definition_or_compatibility");
    }
    if (build_data::is_exotic_catalyst_lane(targetDefinition.definitionIndex, socketLane)) {
        return fail("catalyst_lane_requires_atomic_change");
    }

    // Ownership only matters where the plug is a finite supply the account draws down. A shader is
    // one: pulled from Collections into a profile stack and spent on apply. An ornament is a
    // permanent unlock, so requiring a stack for one would refuse a plug the account already has.
    const bool consumesStack =
        build_data::is_profile_action_source(plugDefinitionIndex, plugDefinition.bucketId)
        && build_data::is_consumed_on_apply(plugDefinitionIndex, plugDefinition.bucketId)
        && !(socketLane < detail.initialPlugIndices.size()
             && detail.initialPlugIndices[socketLane] == plugDefinitionIndex);
    if (consumesStack && !holds_plug_source(snapshot, plugDefinition.definitionHash)) {
        return fail("plug_ownership");
    }

    AccountState chargedAccount = snapshot;
    build_data::material_requirements::Definition materialSet{};
    bool profileChanged = false;
    const std::uint16_t materialSetIndex = plugDefinition.insertionMaterialRequirementSetIndex;
    if (materialSetIndex != build_data::items::kUnavailableMaterialRequirementSetIndex
        && (!build_data::find_material_requirement_set(materialSetIndex, materialSet)
            || materialSet.requirementSetIndex != materialSetIndex
            || !apply_action_materials(snapshot, materialSet, chargedAccount, profileChanged))) {
        return fail("materials");
    }

    // Applying spends the stack the plug came from; the insertion cost above is a separate charge.
    // The authored-cost path refuses any row carrying an instance key, and an action source always
    // has one, so the row keeps its identity until the last unit goes and is released with it.
    if (consumesStack && !spend_plug_source(chargedAccount, plugDefinition.definitionHash)) {
        return fail("plug_stack");
    }
    profileChanged = profileChanged || consumesStack;

    authored_inventory::Sockets authoredSockets{};
    if (target->sockets.policy == authored_inventory::SocketPolicy::nativeDefaults) {
        if (!materialize_native_sockets(detail, authoredSockets)) {
            return fail("native_sockets");
        }
    } else {
        authoredSockets = target->sockets;
        if (authoredSockets.policy != authored_inventory::SocketPolicy::authored
            || authoredSockets.plugCount != detail.ordinarySocketCount
            || !authored_inventory::valid(authoredSockets)) {
            return fail("authored_sockets");
        }
    }
    if (authoredSockets.plugs[socketLane].has_value()
        && *authoredSockets.plugs[socketLane] == plugDefinition.definitionHash) {
        return fail("already_applied");
    }

    // A rolled socket's apply or re-roll plug is an action, not a plug: the service answered it
    // by socketing a result plug from the socket's roll set. The requested plug still decides the
    // pool check and the material charge above; only the plug that lands in the lane changes.
    build_data::items::Definition grantedDefinition = plugDefinition;
    if (classify_rolled_plug(plugDefinition, targetDefinition, socketLane)
        == RolledPlugAction::roll) {
        const std::uint32_t currentPlugHash = authoredSockets.plugs[socketLane].value_or(0);
        const bool reroll = is_rolled_result(currentPlugHash);
        // A re-staging must land on the plug the first staging rolled, so the pinned roll is
        // taken as long as it is still one the fresh roll could have produced.
        RolledPlug rolled{};
        if (pinnedPlugHash != 0) {
            if (pinnedPlugHash == currentPlugHash
                || !pin_rolled_plug(pinnedPlugHash, detail, rolled)) {
                return fail("rolled_plug_pin");
            }
        } else {
            const std::uint64_t seed = targetInstanceSoid
                                       ^ (static_cast<std::uint64_t>(GetTickCount64()) << 8U)
                                       ^ static_cast<std::uint64_t>(before.nextInventorySerial);
            if (!roll_socket_plug(plugDefinition,
                                  detail,
                                  static_cast<std::uint8_t>(before.characterClass),
                                  currentPlugHash,
                                  seed,
                                  rolled)) {
                return fail("rolled_plug_roll");
            }
        }
        if (!build_data::find_item_definition_hash(rolled.plugHash, grantedDefinition)
            || grantedDefinition.definitionHash != rolled.plugHash) {
            return fail("rolled_plug_roll");
        }
        // A result that stands for another plug re-rolls that plug's lane as well: the service
        // swapped the piece's stat perk to the one the result names, which is what moves the
        // stats.
        if (rolled.linkedPerkHash != 0) {
            build_data::items::Definition linkedDefinition{};
            if (rolled.linkedLane == socketLane || rolled.linkedLane >= detail.ordinarySocketCount
                || !build_data::find_item_definition_hash(rolled.linkedPerkHash, linkedDefinition)
                || linkedDefinition.definitionHash != rolled.linkedPerkHash) {
                return fail("rolled_plug_link");
            }
            authoredSockets.plugs[rolled.linkedLane] = rolled.linkedPerkHash;
            report_socket_plug("rolled_plug_link",
                               "ok",
                               reroll ? "reroll" : "apply",
                               before.soid,
                               targetInstanceSoid,
                               targetDefinition.definitionIndex,
                               rolled.linkedLane,
                               linkedDefinition.definitionIndex,
                               targetDefinition.bucketId,
                               linkedDefinition.bucketId,
                               location.equipped,
                               location.index);
        }
        report_socket_plug("rolled_plug",
                           "ok",
                           reroll ? "reroll" : "apply",
                           before.soid,
                           targetInstanceSoid,
                           targetDefinition.definitionIndex,
                           socketLane,
                           grantedDefinition.definitionIndex,
                           targetDefinition.bucketId,
                           grantedDefinition.bucketId,
                           location.equipped,
                           location.index);
    }
    authoredSockets.plugs[socketLane] = grantedDefinition.definitionHash;

    CharacterState after = before;
    authored_inventory::Item* changed = character_item_at(after, location);
    if (changed == nullptr || changed->instanceSoid != target->instanceSoid
        || changed->definitionHash != target->definitionHash || changed->level != target->level
        || changed->quantity != target->quantity
        || changed->mutationSerial != target->mutationSerial) {
        return fail("target_copy");
    }
    changed->sockets = authoredSockets;

    AccountState candidate = chargedAccount;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout afterLoadout{};
    ResolvedPosition beforePosition{};
    ResolvedPosition afterPosition{};
    const family4_loadout::ResolvedItem* resolvedTarget = nullptr;
    for (std::size_t index = 0; index < beforeLoadout.itemCount; ++index) {
        const auto& resolved = beforeLoadout.items[index];
        if (resolved.instance.instanceSoid == targetInstanceSoid
            && resolved.instance.baseDefinitionIndex != targetDefinition.definitionIndex) {
            return fail("before_definition");
        }
    }
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, afterLoadout)
        || !find_resolved_position(beforeLoadout, targetInstanceSoid, beforePosition)
        || !find_resolved_position(afterLoadout, targetInstanceSoid, afterPosition)
        || !same_position(beforePosition, afterPosition)) {
        return fail("candidate_or_position");
    }
    for (std::size_t index = 0; index < afterLoadout.itemCount; ++index) {
        const auto& resolved = afterLoadout.items[index];
        if (resolved.instance.instanceSoid != targetInstanceSoid) {
            continue;
        }
        if (resolvedTarget != nullptr) {
            return fail("duplicate_target");
        }
        resolvedTarget = &resolved;
    }
    if (resolvedTarget == nullptr
        || resolvedTarget->instance.baseDefinitionIndex != targetDefinition.definitionIndex
        || resolvedTarget->instance.ordinarySockets.state
               != middleware::datagen::family4::instance::OrdinarySocketBlockState::present
        || !resolvedTarget->instance.ordinarySockets.plugs[socketLane].has_value()
        || *resolvedTarget->instance.ordinarySockets.plugs[socketLane]
               != grantedDefinition.definitionIndex) {
        return fail("after_socket");
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.beforeProfileItems = snapshot.profileItems;
    mutation.afterProfileItems = chargedAccount.profileItems;
    mutation.accountSoid = snapshot.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.targetInstanceSoid = targetInstanceSoid;
    mutation.targetDefinitionHash = targetDefinition.definitionHash;
    mutation.plugDefinitionHash = grantedDefinition.definitionHash;
    mutation.materialRequirementSetHash = materialSet.requirementSetHash;
    mutation.characterIndex = characterIndex;
    mutation.expectedProfileItemCount = snapshot.profileItemCount;
    mutation.afterProfileItemCount = chargedAccount.profileItemCount;
    mutation.itemIndex = location.index;
    mutation.targetDefinitionIndex = targetDefinition.definitionIndex;
    mutation.plugDefinitionIndex = grantedDefinition.definitionIndex;
    mutation.requestedPlugDefinitionIndex = plugDefinitionIndex;
    mutation.materialRequirementSetIndex = materialSetIndex;
    mutation.socketLane = socketLane;
    mutation.targetBucketId = targetDefinition.bucketId;
    mutation.plugBucketId = grantedDefinition.bucketId;
    mutation.materialRequirementCount = materialSet.requirementCount;
    mutation.profileChanged = profileChanged;
    mutation.targetEquipped = location.equipped;
    mutation.prepared = true;
    return true;
}

/** Stages one complete accumulated item-state value without moving or recreating the item. */
[[nodiscard]] bool stage_item_state(const AccountState& snapshot,
                                    std::size_t characterIndex,
                                    std::uint64_t targetInstanceSoid,
                                    std::uint16_t targetDefinitionIndex,
                                    std::uint32_t flags,
                                    PendingItemState& mutation) noexcept {
    mutation = {};
    if (!account::valid(snapshot) || characterIndex >= snapshot.characterCount
        || targetInstanceSoid == 0 || !authored_inventory::valid_item_state(flags)) {
        return false;
    }
    const CharacterState& before = snapshot.characters[characterIndex];
    if (!before.selected || before.soid == 0) {
        return false;
    }

    CharacterItemLocation location{};
    family4_loadout::ResolvedLoadout beforeLoadout{};
    if (!find_character_item_location(before, targetInstanceSoid, location)
        || !family4_loadout::resolve(snapshot, characterIndex, beforeLoadout)) {
        return false;
    }
    const authored_inventory::Item* target = character_item_at(before, location);
    build_data::items::Definition definition{};
    ResolvedPosition beforePosition{};
    if (target == nullptr || target->flags == flags
        || !build_data::find_item_definition_hash(target->definitionHash, definition)
        || definition.definitionHash != target->definitionHash
        || definition.definitionIndex != targetDefinitionIndex
        || !find_resolved_position(beforeLoadout, targetInstanceSoid, beforePosition)) {
        return false;
    }

    CharacterState after = before;
    authored_inventory::Item* changed = character_item_at(after, location);
    if (changed == nullptr || !same_stationary_item(*changed, *target)) {
        return false;
    }
    changed->flags = flags;

    AccountState candidate = snapshot;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout afterLoadout{};
    ResolvedPosition afterPosition{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, afterLoadout)
        || !find_resolved_position(afterLoadout, targetInstanceSoid, afterPosition)
        || !same_position(beforePosition, afterPosition)) {
        return false;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.characterSoid = before.soid;
    mutation.targetInstanceSoid = targetInstanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.itemIndex = location.index;
    mutation.targetDefinitionIndex = targetDefinitionIndex;
    mutation.beforeFlags = target->flags;
    mutation.afterFlags = flags;
    mutation.targetEquipped = location.equipped;
    mutation.prepared = true;
    return true;
}

} // namespace runtime::detail
} // namespace sunrise::state
