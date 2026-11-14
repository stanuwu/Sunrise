#pragma once

#include "../../middleware/bap/activity_message/squad_objective_state.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host::actor_program {

/** Squad Auth ordinals identify candidate counts, profile, generation and active state. */
inline constexpr std::size_t kCountsField = 3, kProfileField = 5, kGenerationField = 6,
                             kActiveField = 18;
/** Logical active one is encoded with bias one. */
inline constexpr std::uint32_t kActiveWire = 2, kInactiveWire = 1;
/** The squad requested-count array has a four-bit length and signed 32-bit elements. */
inline constexpr std::uint8_t kCountWidth = 4, kElementWidth = 32;

/** Delivered actor state belongs to the activity and wire identity, not an advertisement. */
[[nodiscard]] inline const PendingScriptableOverride*
retained(const detail::Instance& instance, const ScriptableTarget& target) noexcept {
    for (const auto& body : instance.scriptableAuthEstate) {
        const auto& row = body.target;
        if (row.objectTag == target.objectTag && row.registryKey == target.registryKey
            && row.slotIndex == target.slotIndex && row.slotType == target.slotType
            && row.authSchema == target.authSchema && body.byteCount != 0
            && body.byteCount <= body.body.size()) {
            return &body;
        }
    }
    return nullptr;
}

/**
 * Reads the actor's last spawn revision: the committed one, or the delivered body's if later.
 * @return False when the delivered body does not decode.
 */
[[nodiscard]] inline bool committed_spawn_generation(const detail::Instance& instance,
                                                     const ScriptableTarget& target,
                                                     const detail::ScriptableGuard& guard,
                                                     std::uint32_t& output) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    output = guard.type2SpawnGeneration;
    const auto* previous = retained(instance, target);
    if (previous == nullptr) {
        return true;
    }
    auth::Type2ProgramLayout layout{};
    if (!auth::inspect_type2_program(
            std::span(previous->body).first(previous->byteCount), previous->bitCount, layout)) {
        return false;
    }
    output = (std::max)(output, layout.spawnGeneration);
    return true;
}

/** Creation needs a transported reservation with an active, authored squad candidate. */
[[nodiscard]] inline bool source_ready(const detail::Instance& instance,
                                       const ScriptableTarget& target) noexcept {
    namespace patch = middleware::bap::activity_message::mission_auth_patch;
    namespace objective = middleware::bap::activity_message::squad_objective;
    namespace fields = middleware::bap::activity_message::auth_fields;
    const auto* source = retained(instance, target);
    if (source == nullptr) {
        return false;
    }
    const auto body = std::span(source->body).first(source->byteCount);
    patch::Layout layout{};
    objective::State state{};
    if (!patch::parse(objective::kSchema, body, source->bitCount, layout)
        || !objective::read_state(body, source->bitCount, state) || !state.reserved
        || !layout.fields[kCountsField].present || !layout.fields[kProfileField].present
        || !layout.fields[kGenerationField].present) {
        return false;
    }
    middleware::encoding::bits::Reader active(body);
    std::uint64_t value = 0;
    if (!active.skip(layout.fields[kActiveField].offset)
        || !active.read(static_cast<std::uint8_t>(patch::kSquadRules[kActiveField].width), value)
        || value != kActiveWire) {
        return false;
    }
    middleware::encoding::bits::Reader generation(body);
    if (!generation.skip(layout.fields[kGenerationField].offset + fields::kPresenceWidth)
        || !generation.read(fields::kCounterWidth, value) || value == 0) {
        return false;
    }
    middleware::encoding::bits::Reader counts(body);
    std::uint64_t count = 0;
    if (!counts.skip(layout.fields[kCountsField].offset + fields::kPresenceWidth)
        || !counts.read(kCountWidth, count)) {
        return false;
    }
    bool positive = false;
    for (std::uint64_t index = 0; index < count; ++index) {
        if (!counts.read(kElementWidth, value) || value < fields::kSigned32Bias) {
            return false;
        }
        positive = positive || value > fields::kSigned32Bias;
    }
    return positive;
}

/** The Host lock stays held while the retained root and both counters are replaced. */
[[nodiscard]] inline bool encode(const detail::Instance& instance,
                                 const detail::ScriptableRequest& request,
                                 const detail::ScriptableGuard& guard,
                                 PendingScriptableOverride& pending,
                                 std::size_t& written) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    const auto* previous = retained(instance, request.target);
    const bool retire = request.kind == ScriptableOverrideKind::combatantRetirement;
    if ((request.active && !source_ready(instance, request.programSource))
        || (!request.active && !retire && previous == nullptr)) {
        return false;
    }
    std::size_t bits = 0;
    std::uint32_t generation = 0;
    const bool encoded = auth::replace_type2_atoms(
        previous != nullptr ? std::span(previous->body).first(previous->byteCount)
                            : std::span<const std::byte>{},
        previous != nullptr ? previous->bitCount : 0,
        std::span(request.authBody).first(request.authByteCount),
        request.authBitCount,
        guard.type2AtomGeneration,
        guard.type2SpawnGeneration,
        request.active,
        pending.body,
        written,
        bits,
        generation,
        pending.actorSpawnGeneration,
        retire);
    pending.bitCount = static_cast<std::uint16_t>(bits);
    pending.generation = generation;
    return encoded;
}

/** Replaces the live actor's controls; the revision follows the transported body's. */
[[nodiscard]] inline bool encode_controls(const detail::Instance& instance,
                                          const detail::ScriptableRequest& request,
                                          PendingScriptableOverride& pending,
                                          std::size_t& written) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    const auto* previous = retained(instance, request.target);
    if (previous == nullptr) {
        return false;
    }
    std::size_t bits = 0;
    std::uint32_t revision = 0;
    const bool encoded =
        auth::replace_type2_controls(std::span(previous->body).first(previous->byteCount),
                                     previous->bitCount,
                                     std::span(request.authBody).first(request.authByteCount),
                                     request.authBitCount,
                                     pending.body,
                                     written,
                                     bits,
                                     revision);
    pending.bitCount = static_cast<std::uint16_t>(bits);
    pending.generation = revision;
    return encoded;
}

/**
 * Every squad a manifest names must hold a transported reservation on the actor's own object.
 * The manifest names squads by registry key and index, which with the actor's object tag is the
 * whole identity a retained body is found by.
 */
[[nodiscard]] inline bool
passengers_reserved(const detail::Instance& instance,
                    const ScriptableTarget& actor,
                    const middleware::bap::activity_message::scriptable_auth::Type2ProgramLayout&
                        manifest) noexcept {
    namespace squad = middleware::bap::activity_message::squad_auth;
    for (std::size_t index = 0; index < manifest.deliveryCount; ++index) {
        const auto& passenger = manifest.deliverySources[index];
        ScriptableTarget source{};
        source.objectTag = actor.objectTag;
        source.registryKey = passenger.registryKey;
        source.authSchema = squad::kSchema;
        source.slotIndex = static_cast<std::uint16_t>(passenger.slotIndex);
        source.slotType = squad::kSlotType;
        if (passenger.registryKey != actor.registryKey || !source_ready(instance, source)) {
            return false;
        }
    }
    return true;
}

/** Merges a passenger manifest into the live actor; the Host owns its manifest revision. */
[[nodiscard]] inline bool encode_delivery(const detail::Instance& instance,
                                          const detail::ScriptableRequest& request,
                                          const detail::ScriptableGuard& guard,
                                          PendingScriptableOverride& pending,
                                          std::size_t& written) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    const auto* previous = retained(instance, request.target);
    const auto manifest = std::span(request.authBody).first(request.authByteCount);
    auth::Type2ProgramLayout layout{};
    if (previous == nullptr || !auth::inspect_type2_program(manifest, request.authBitCount, layout)
        || !passengers_reserved(instance, request.target, layout)) {
        return false;
    }
    std::size_t bits = 0;
    std::uint32_t revision = 0;
    const bool encoded =
        auth::replace_type2_delivery(std::span(previous->body).first(previous->byteCount),
                                     previous->bitCount,
                                     manifest,
                                     request.authBitCount,
                                     guard.type2DeliveryRevision,
                                     pending.body,
                                     written,
                                     bits,
                                     revision);
    pending.bitCount = static_cast<std::uint16_t>(bits);
    pending.generation = revision;
    return encoded;
}

} // namespace sunrise::server::activity::host::actor_program
