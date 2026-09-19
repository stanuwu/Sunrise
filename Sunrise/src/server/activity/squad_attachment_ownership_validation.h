#pragma once

#include <algorithm>

#include "../../middleware/bap/activity_message/squad_attachment_auth.h"
#include "../../state/activity_sdk/attachment_compatibility.h"
#include "host_runtime.h"

namespace sunrise::server::activity::host::attachments {

/**
 * Requires a type-26 attachment and the squad source that shares its object, registry, roster group
 * and state-local placement. Any authored pair with that shape is admitted; no content is named.
 */
[[nodiscard]] inline bool same_owner_pair(const ScriptableTarget& target,
                                          const SquadAttachmentOwnership& owned) noexcept {
    const auto& source = owned.source;
    return owned.sdkBuildSha256 != std::array<std::byte, 32>{}
           && state::activity_sdk::attachment_compatibility::supports(owned.sdkPayloadSha256)
           && owned.sourceSpawnGeneration > 0 && owned.sourceSpawnGeneration <= 0x7FFFFFFFU
           && target.slotType == 26
           && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType26Schema
           && source.objectTag == target.objectTag && source.registryKey == target.registryKey
           && source.slotType == state::activity_sdk::format::kSquadSlotType
           && source.authSchema == state::activity_sdk::format::kSquadAuthSchema
           && source.sdkObjectIndex == target.sdkObjectIndex
           && source.stateLocalRoster == target.stateLocalRoster
           && source.stateLocalRegion == target.stateLocalRegion
           && source.rosterGroupIndex == target.rosterGroupIndex;
}

/** Full wire identity, independent of the occurrence metadata required by owned attachments. */
[[nodiscard]] inline bool same_ref(const ScriptableTarget& left,
                                   const ScriptableTarget& right) noexcept {
    return left.objectTag == right.objectTag && left.registryKey == right.registryKey
           && left.slotType == right.slotType && left.slotIndex == right.slotIndex;
}

/**
 * @return The placement spawn generation a retained squad row stands for. An objective assignment
 * replaces the placement row with a squadObjective patch that carries the generation forward.
 */
[[nodiscard]] inline bool source_spawn_generation(const PendingScriptableOverride& row,
                                                  std::uint64_t& value) noexcept {
    if (row.byteCount == 0 || row.byteCount > row.body.size()) {
        return false;
    }
    if (row.kind == ScriptableOverrideKind::squad) {
        value = row.generation;
        return true;
    }
    if (row.kind == ScriptableOverrideKind::squadObjective && row.squadSpawnGeneration != 0) {
        value = row.squadSpawnGeneration;
        return true;
    }
    return false;
}

/** @return True when the body exactly represents the owned selection and authored prefix. */
[[nodiscard]] inline bool matches_body(const SquadAttachmentOwnership& owned,
                                       std::span<const std::byte> body,
                                       std::size_t bitCount) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    std::array<std::byte, auth::kType26MaximumByteCount> expected{};
    std::size_t written = 0, bits = 0;
    const auth::Type26SquadSelection selection =
        owned.active ? auth::Type26SquadSelection{owned.source.registryKey,
                                                  static_cast<std::int16_t>(owned.source.slotIndex),
                                                  true}
                     : auth::Type26SquadSelection{};
    return auth::encode_type26_squad_selection(selection, expected, written, bits)
           && bits == bitCount && body.size() == written
           && std::equal(body.begin(), body.end(), expected.begin());
}

/**
 * Checks fresh claim/update/clear against delivered source and attachment lifetimes.
 * The caller owns binding/held-region locks and must repeat this before transport publication.
 */
[[nodiscard]] inline bool admits(const ScriptableTarget& target,
                                 const std::optional<SquadAttachmentOwnership>& requested,
                                 std::span<const PendingScriptableOverride> estate,
                                 std::span<const std::byte> body,
                                 std::size_t bitCount) noexcept {
    if (!requested) {
        // Occurrence metadata can change for shared unowned scriptables. Only retained
        // attachment ownership requires an owned update, including after an owned clear.
        for (const auto& row : estate) {
            if (same_ref(row.target, target) && row.squadAttachment) {
                return false;
            }
        }
        return true;
    }
    const PendingScriptableOverride* previous = nullptr;
    for (const auto& row : estate) {
        if (same_ref(row.target, target)) {
            if (previous != nullptr || row.target != target) {
                return false;
            }
            previous = &row;
        }
    }
    const auto& owned = *requested;
    if (!same_owner_pair(target, owned)) {
        return false;
    }
    const PendingScriptableOverride* source = nullptr;
    for (const auto& row : estate) {
        if (same_ref(row.target, owned.source)) {
            if (source != nullptr || row.target != owned.source) {
                return false;
            }
            source = &row;
        }
    }
    std::uint64_t sourceSpawn = 0;
    if (source == nullptr || !source_spawn_generation(*source, sourceSpawn)
        || sourceSpawn != owned.sourceSpawnGeneration) {
        return false;
    }
    if (previous == nullptr) {
        if (!owned.active || owned.previousRevision != 0) {
            return false;
        }
    } else {
        if (!previous->squadAttachment || previous->revision != owned.previousRevision
            || previous->squadAttachment->sdkBuildSha256 != owned.sdkBuildSha256
            || previous->squadAttachment->sdkPayloadSha256 != owned.sdkPayloadSha256
            || previous->squadAttachment->source != owned.source
            || ((previous->squadAttachment->active || !owned.active)
                && previous->squadAttachment->sourceSpawnGeneration
                       != owned.sourceSpawnGeneration)) {
            return false;
        }
        if (previous->kind != ScriptableOverrideKind::sdkAuth
            || previous->byteCount > previous->body.size()
            || !matches_body(*previous->squadAttachment,
                             std::span(previous->body).first(previous->byteCount),
                             previous->bitCount)) {
            return false;
        }
    }
    return matches_body(owned, body, bitCount);
}

/** Keeps an active attachment on its selected lifetime until an owned clear is delivered. */
[[nodiscard]] inline bool
permits_source_write(const PendingScriptableOverride& pending,
                     std::span<const PendingScriptableOverride> estate) noexcept {
    for (const auto& row : estate) {
        // An objective assignment never changes the selected lifetime; a re-placement must keep it.
        if (row.squadAttachment && row.squadAttachment->active
            && same_ref(row.squadAttachment->source, pending.target)
            && pending.kind != ScriptableOverrideKind::squadObjective
            && (pending.target != row.squadAttachment->source
                || pending.kind != ScriptableOverrideKind::squad
                || pending.generation != row.squadAttachment->sourceSpawnGeneration)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::server::activity::host::attachments
