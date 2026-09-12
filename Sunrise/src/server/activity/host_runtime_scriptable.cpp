#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

#include "../../middleware/bap/activity_message/mission_auth_patch.h"
#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../state/activity/mission/runtime.h"
#include "../../state/activity/runtime.h"
#include "../gameplay/squad_entity_retirement.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace {

namespace auth = middleware::bap::activity_message::scriptable_auth;
namespace scene = middleware::bap::activity_message::sensor_auth_update;
namespace squad = middleware::bap::activity_message::squad_auth;
using namespace detail;

/** Authored-scene activation generations are positive signed 32-bit values. */
constexpr std::uint32_t kMaximumAuthoredSceneGeneration = 0x7FFFFFFFU;

/** @return True when both values name the same full ClientRef slot. */
[[nodiscard]] bool same_target(const ScriptableTarget& left,
                               const ScriptableTarget& right) noexcept {
    return left.objectTag == right.objectTag && left.registryKey == right.registryKey
           && left.authSchema == right.authSchema && left.rosterGroupIndex == right.rosterGroupIndex
           && left.rosterSlotOffset == right.rosterSlotOffset && left.slotIndex == right.slotIndex
           && left.sdkObjectIndex == right.sdkObjectIndex
           && left.stateLocalRegion == right.stateLocalRegion && left.slotType == right.slotType
           && left.stateLocalRoster == right.stateLocalRoster;
}

/** @return True when both routes resolve to the same wire ClientRef. */
[[nodiscard]] bool same_client_ref(const ScriptableTarget& left,
                                   const ScriptableTarget& right) noexcept {
    return left.objectTag == right.objectTag && left.registryKey == right.registryKey
           && left.slotIndex == right.slotIndex && left.slotType == right.slotType;
}

/** @return True when both retained generated groups are byte-for-byte identical in used fields. */
[[nodiscard]] bool same_group(const state::build_data::scenarios::RosterGroup& left,
                              const state::build_data::scenarios::RosterGroup& right) noexcept {
    if (!state::build_data::scenarios::valid_roster_group(left)
        || !state::build_data::scenarios::valid_roster_group(right)
        || left.registryKey != right.registryKey || left.objectTag != right.objectTag
        || left.slotCount != right.slotCount) {
        return false;
    }
    for (std::size_t index = 0; index < left.slotCount; ++index) {
        if (left.slotTypes[index] != right.slotTypes[index]
            || left.slotFlags[index] != right.slotFlags[index]
            || left.slotIndices[index] != right.slotIndices[index]) {
            return false;
        }
    }
    return true;
}

/** @return True when one caller still owns the exact unarmed lane held by this instance. */
[[nodiscard]] bool same_reservation(const ScriptableOutputReservation& left,
                                    const ScriptableOutputReservation& right) noexcept {
    return same_binding(left.binding, right.binding)
           && left.resetGeneration == right.resetGeneration && left.token == right.token
           && left.revision == right.revision && left.intentSequence == right.intentSequence
           && left.resetGeneration != 0 && left.token != 0 && left.revision != 0;
}

/** Clears one unarmed reservation without touching the committed output counter. */
void clear_reservation(Instance& instance) noexcept {
    instance.scriptableReservation = {};
    instance.view.scriptableReservedRevision = 0;
    instance.view.scriptableReservationPending = false;
}

/** @return True when the target carries one exact supported type/schema pair. */
[[nodiscard]] bool supported_target(const ScriptableTarget& target) noexcept {
    const bool generatedStateLocal = target.stateLocalRoster && target.stateLocalRegion >= 0
                                     && target.rosterGroupIndex == kGeneratedRosterGroupIndex
                                     && target.sdkObjectIndex != kNoSdkObjectIndex;
    const bool canonical = !target.stateLocalRoster && target.stateLocalRegion < 0
                           && target.rosterGroupIndex != kGeneratedRosterGroupIndex
                           && target.sdkObjectIndex == kNoSdkObjectIndex;
    return target.slotType <= scene::kMaximumSlotType
           && target.slotIndex <= scene::kMaximumSlotIndex && target.authSchema != 0
           && (generatedStateLocal || canonical);
}

/** Finds unused storage for a full-slot guard without mutating it. */
[[nodiscard]] ScriptableGuard* free_guard(Instance& instance) noexcept {
    for (ScriptableGuard& guard : instance.scriptableGuards) {
        if (!guard.occupied) {
            return &guard;
        }
    }
    return nullptr;
}

/** @return True when this kind carries no push-side behaviour beyond its retained body. */
[[nodiscard]] bool tail_eligible(ScriptableOverrideKind kind) noexcept {
    return kind != ScriptableOverrideKind::lifetime && kind != ScriptableOverrideKind::squad;
}

/** @return True when this instance already holds a committed body for the same ClientRef. */
[[nodiscard]] bool pending_holds_target(const Instance& instance,
                                        const ScriptableTarget& target) noexcept {
    if (instance.view.outputPending && same_client_ref(instance.pendingScriptable.target, target)) {
        return true;
    }
    for (std::size_t index = 0; index < instance.pendingScriptableTailCount; ++index) {
        if (same_client_ref(instance.pendingScriptableTail[index].target, target)) {
            return true;
        }
    }
    return false;
}

/** @return True when another body may commit before the pending push carries the head out. */
[[nodiscard]] bool tail_has_room(const Instance& instance,
                                 const ScriptableRequest& request) noexcept {
    return instance.view.outputPending && tail_eligible(request.kind)
           && instance.view.outputKind == OutputKind::scriptableOverride
           && tail_eligible(instance.pendingScriptable.kind)
           && instance.pendingScriptableTailCount < instance.pendingScriptableTail.size()
           && !pending_holds_target(instance, request.target);
}

} // namespace

namespace detail {

/** @return The next positive authored-scene generation without changing the guard. */
[[nodiscard]] bool next_authored_scene_generation(std::uint32_t last,
                                                  std::uint32_t& output) noexcept {
    output = 0;
    if (last >= kMaximumAuthoredSceneGeneration) {
        return false;
    }
    output = last + 1;
    return true;
}

/** @return True when one carried group contains the target's exact selected auth slot. */
[[nodiscard]] bool
valid_state_local_group(const ScriptableTarget& target,
                        const state::build_data::scenarios::RosterGroup& group) noexcept {
    const std::size_t slot = target.rosterSlotOffset;
    return target.stateLocalRoster && target.stateLocalRegion >= 0
           && target.rosterGroupIndex == kGeneratedRosterGroupIndex
           && target.sdkObjectIndex != kNoSdkObjectIndex
           && state::build_data::scenarios::valid_roster_group(group) && group.objectTag != 0
           && group.objectTag == target.objectTag && group.registryKey == target.registryKey
           && slot < group.slotCount && group.slotTypes[slot] == target.slotType
           && group.slotIndices[slot] == target.slotIndex
           && (group.slotFlags[slot] & state::build_data::scenarios::kSlotAuthFlag) != 0;
}

/** @return True when the bit count and the body agree to within one trailing byte. */
[[nodiscard]] bool valid_auth_storage(std::span<const std::byte> body,
                                      std::size_t bitCount) noexcept {
    if (body.empty() || bitCount > body.size() * 8U || bitCount + 7U < body.size() * 8U) {
        return false;
    }
    const std::size_t trailingBits = bitCount % 8U;
    if (trailingBits == 0) {
        return true;
    }
    const std::uint8_t paddingMask =
        static_cast<std::uint8_t>((std::uint16_t{1} << (8U - trailingBits)) - 1U);
    return (std::to_integer<std::uint8_t>(body.back()) & paddingMask) == 0;
}

/** Finds one committed full-slot guard while the runtime lock is held. */
[[nodiscard]] ScriptableGuard* find_guard(Instance& instance,
                                          const ScriptableTarget& target) noexcept {
    for (ScriptableGuard& guard : instance.scriptableGuards) {
        if (guard.occupied && same_target(guard.target, target)) {
            return &guard;
        }
    }
    return nullptr;
}

/** @return True when a transport acknowledgement names the retained body byte-for-byte. */
[[nodiscard]] bool same_pending(const PendingScriptableOverride& left,
                                const PendingScriptableOverride& right) noexcept {
    return left.squadRetirement == right.squadRetirement && left.revision == right.revision
           && left.kind == right.kind && same_target(left.target, right.target)
           && left.generation == right.generation
           && left.expectedActivityClientGeneration == right.expectedActivityClientGeneration
           && left.sequence == right.sequence && left.dialogueSequence == right.dialogueSequence
           && left.dialogueCue == right.dialogueCue && left.bitCount == right.bitCount
           && left.byteCount == right.byteCount && left.channel == right.channel
           && left.lifetimeState == right.lifetimeState && left.body == right.body
           && left.sdkCompiled == right.sdkCompiled
           && (!left.target.stateLocalRoster
               || same_group(left.stateLocalRosterGroup, right.stateLocalRosterGroup));
}

/** Replaces one delivered full-ClientRef body, or appends its first value. */
[[nodiscard]] bool retain_scriptable_auth(Instance& instance,
                                          const PendingScriptableOverride& pending,
                                          std::uint64_t sourceGeneration) noexcept {
    if (pending.kind == ScriptableOverrideKind::lifetime) {
        return true;
    }
    if (pending.byteCount == 0 || pending.byteCount > pending.body.size()) {
        return false;
    }
    PendingScriptableOverride owned = pending;
    if (owned.expectedActivityClientGeneration == 0) {
        owned.expectedActivityClientGeneration = sourceGeneration;
    }
    for (PendingScriptableOverride& retained : instance.scriptableAuthEstate) {
        if (same_client_ref(retained.target, pending.target)) {
            retained = owned;
            return true;
        }
    }
    try {
        instance.scriptableAuthEstate.push_back(owned);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

/** A sequence needs the current client's delivered, enabled squad binding. */
[[nodiscard]] const PendingScriptableOverride*
retained_sequence_combatant(const Instance& instance, const ScriptableRequest& request) noexcept {
    for (const auto& retained : instance.scriptableAuthEstate) {
        if (!same_client_ref(retained.target, request.target)
            || retained.expectedActivityClientGeneration != request.expectedActivityClientGeneration
            || retained.byteCount > retained.body.size()) {
            continue;
        }
        auth::Type2ProgramLayout layout{};
        return auth::inspect_type2_program(
                   std::span(retained.body).first(retained.byteCount), retained.bitCount, layout)
                       && layout.enabled && (layout.bindingWire == 2 || layout.bindingWire == 4)
                       && layout.generation < squad::kMaximumGeneration
                   ? &retained
                   : nullptr;
    }
    return nullptr;
}

/** Queues one validated scriptable request in the shared ordered control lane. */
[[nodiscard]] bool enqueue_request(ScriptableRequest request,
                                   const ScriptableOutputReservation* reservation) noexcept {
    // A lifetime request changes activity state, so it carries no ClientRef slot to validate.
    const bool untargeted = request.kind == ScriptableOverrideKind::lifetime;
    if ((!untargeted && !supported_target(request.target))
        || !state::activity::binding_matches(request.binding)) {
        return false;
    }
    if (reservation != nullptr && !same_binding(reservation->binding, request.binding)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(request.binding);
    const bool ownsReservation =
        reservation != nullptr && instance != nullptr && instance->view.active
        && instance->view.scriptableReservationPending
        && same_reservation(instance->scriptableReservation, *reservation)
        && instance->view.scriptableRevision != (std::numeric_limits<std::uint64_t>::max)()
        && instance->view.scriptableRevision + 1 == reservation->revision;
    const bool burst = request.burstMember && request.expectedRevision != 0
                       && tail_eligible(request.kind) && instance != nullptr;
    if ((request.kind == ScriptableOverrideKind::combatantSequence
         && (instance == nullptr || retained_sequence_combatant(*instance, request) == nullptr))
        || (!burst && has_queued_control(request.binding))
        || (!burst && instance != nullptr && instance->view.outputPending)
        || (!burst && reservation != nullptr && !ownsReservation)
        || (!burst && reservation == nullptr && instance != nullptr
            && instance->view.scriptableReservationPending)) {
        ++g_refusedControls;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (reservation != nullptr) {
        request.expectedRevision = reservation->revision;
        request.expectedIntentSequence = reservation->intentSequence;
    }
    PendingInput pending{};
    pending.kind = PendingKind::scriptableControl;
    pending.scriptableControl = request;
    if (!append_pending(pending)) {
        ++g_refusedControls;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    ++g_queuedControls;
    if (reservation != nullptr) {
        clear_reservation(*instance);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Clears one exact pending body while the runtime lock is held. */
void cancel_pending(Instance& instance,
                    const state::activity::SessionBinding& binding,
                    std::uint64_t expectedRevision) noexcept {
    const std::uint64_t now = GetTickCount64();
    Event event{};
    event.binding = binding;
    event.tick = now;
    event.kind = EventKind::scriptableOverrideCanceled;
    for (std::size_t index = 0; index < instance.pendingScriptableTailCount; ++index) {
        event.scriptableRevision = instance.pendingScriptableTail[index].revision;
        append_event(event);
    }
    instance.pendingScriptableTail.fill({});
    instance.pendingScriptableTailCount = 0;
    instance.pendingScriptable = {};
    instance.view.outputPending = false;
    instance.view.outputKind = OutputKind::none;
    instance.view.outputStatus = OutputStatus::canceled;
    event.scriptableRevision = expectedRevision;
    append_event(event);
    instance.view.lastEventSequence = g_sequence;
}

/** Assigns and encodes one typed counter without committing it before transport staging. */
void apply_scriptable_control(const ScriptableRequest& request, std::uint64_t now) noexcept {
    Event event{};
    event.binding = request.binding;
    event.tick = now;
    event.kind = EventKind::operatorRefused;
    Instance* const instance = find_instance(request.binding);
    const bool durableAssignment =
        request.expectedIntentSequence == 0
        || state::activity::mission::intent_output_assigned(
            request.binding, request.expectedIntentSequence, request.expectedRevision);
    const bool joinsTail = instance != nullptr && request.burstMember
                           && tail_has_room(*instance, request)
                           && request.expectedRevision == instance->view.scriptableRevision;
    if (instance == nullptr || !instance->view.active
        || (instance->view.outputPending && !joinsTail) || !durableAssignment
        || instance->view.scriptableRevision == (std::numeric_limits<std::uint64_t>::max)()
        || (request.expectedRevision != 0 && !joinsTail
            && instance->view.scriptableRevision + 1 != request.expectedRevision)) {
        ++g_refusedControls;
        append_event(event);
        if (instance != nullptr) {
            instance->view.lastEventSequence = g_sequence;
        }
        return;
    }

    // A lifetime request owns no ClientRef slot, so it takes no full-slot counter guard.
    const bool untargeted = request.kind == ScriptableOverrideKind::lifetime;
    ScriptableGuard* guard = untargeted ? nullptr : find_guard(*instance, request.target);
    ScriptableGuard candidate{};
    if (!untargeted) {
        if (guard == nullptr) {
            guard = free_guard(*instance);
            candidate.target = request.target;
            candidate.occupied = true;
        } else {
            candidate = *guard;
        }
    }
    PendingScriptableOverride pending{};
    pending.target = request.target;
    pending.stateLocalRosterGroup = request.stateLocalRosterGroup;
    pending.revision = request.expectedRevision == 0 ? instance->view.scriptableRevision + 1
                                                     : request.expectedRevision;
    pending.kind = request.kind;
    pending.squadRetirement = request.squadRetirement;
    pending.expectedActivityClientGeneration = request.expectedActivityClientGeneration;
    std::size_t written = 0;
    std::size_t writtenBits = 0;
    bool encoded = untargeted || guard != nullptr;
    if (encoded && request.kind == ScriptableOverrideKind::lifetime) {
        pending.lifetimeState = request.lifetimeState;
    } else if (encoded && request.kind == ScriptableOverrideKind::squad) {
        std::uint32_t generation = 0;
        encoded = request.requestedCountLength <= request.requestedCounts.size();
        if (encoded) {
            const std::span<const std::int32_t> counts(request.requestedCounts.data(),
                                                       request.requestedCountLength);
            encoded = squad::next_generation(candidate.squad, generation)
                      && squad::encode({counts,
                                        generation,
                                        request.squadMode,
                                        request.nameHash,
                                        request.squadAuthoredProfile},
                                       candidate.squad,
                                       pending.body,
                                       written,
                                       writtenBits);
        }
        pending.generation = generation;
        if (writtenBits <= (std::numeric_limits<std::uint16_t>::max)()) {
            pending.bitCount = static_cast<std::uint16_t>(writtenBits);
        } else {
            encoded = false;
        }
    } else if (encoded && request.kind == ScriptableOverrideKind::combatantChannel) {
        std::uint32_t revision = 0;
        encoded = auth::next_type2_revision(candidate.type2, revision);
        candidate.type2.revision = revision;
        encoded =
            encoded && auth::set_type2_channel(candidate.type2, request.channelHash, request.value)
            && auth::encode_type2_channels(candidate.type2, pending.body, written, writtenBits);
        if (writtenBits <= (std::numeric_limits<std::uint16_t>::max)()) {
            pending.bitCount = static_cast<std::uint16_t>(writtenBits);
        } else {
            encoded = false;
        }
        pending.channelHash = request.channelHash;
        pending.channelValue = request.value;
        pending.generation = revision;
    } else if (encoded && request.kind == ScriptableOverrideKind::combatantBinding) {
        std::uint32_t revision = 0;
        encoded = auth::next_type2_revision(candidate.type2, revision);
        candidate.type2.revision = revision;
        candidate.type2.actorBinding = auth::Type2ActorBinding::squadMember;
        encoded =
            encoded
            && auth::encode_type2_channels(candidate.type2, pending.body, written, writtenBits);
        if (writtenBits <= (std::numeric_limits<std::uint16_t>::max)()) {
            pending.bitCount = static_cast<std::uint16_t>(writtenBits);
        } else {
            encoded = false;
        }
        pending.generation = revision;
    } else if (encoded && request.kind == ScriptableOverrideKind::combatantSequence) {
        const auto* const retained = retained_sequence_combatant(*instance, request);
        std::size_t bits = 0;
        std::uint32_t generation = 0;
        encoded =
            retained != nullptr
            && auth::replace_type2_sequence(std::span(retained->body).first(retained->byteCount),
                                            retained->bitCount,
                                            request.sequenceHash,
                                            candidate.type2AtomGeneration,
                                            pending.body,
                                            written,
                                            bits,
                                            generation);
        pending.bitCount = static_cast<std::uint16_t>(bits);
        pending.generation = generation;
    } else if (encoded && request.kind == ScriptableOverrideKind::object) {
        pending.bitCount = static_cast<std::uint16_t>(auth::kType4BitCount);
        std::int32_t generation = 0;
        encoded = auth::next_type4_generation(candidate.type4, generation)
                  && auth::encode_type4({generation, request.entryIndex, request.active},
                                        candidate.type4,
                                        pending.body,
                                        written);
        pending.generation = static_cast<std::uint64_t>(generation);
    } else if (encoded && request.kind == ScriptableOverrideKind::sequence) {
        pending.bitCount = static_cast<std::uint16_t>(auth::kType5BitCount);
        std::uint8_t revision = 0;
        encoded = auth::next_type5_revision(candidate.type5, revision)
                  && auth::encode_type5({revision}, candidate.type5, pending.body, written);
        pending.generation = revision;
    } else if (encoded && request.kind == ScriptableOverrideKind::cinematic) {
        pending.bitCount = static_cast<std::uint16_t>(auth::kType6BitCount);
        std::uint32_t generation = 0;
        encoded = auth::next_type6_generation(candidate.type6, generation)
                  && auth::encode_type6(
                      {generation, request.active}, candidate.type6, pending.body, written);
        pending.generation = generation;
    } else if (encoded && request.kind == ScriptableOverrideKind::performance) {
        pending.bitCount = static_cast<std::uint16_t>(auth::kType42BitCount);
        std::int32_t generation = 0;
        auth::Type42Preset preset{};
        preset.nameHash = request.nameHash.value_or(0);
        encoded = auth::next_type42_generation(candidate.type42, generation);
        preset.generation = generation;
        encoded = encoded && auth::encode_type42(preset, candidate.type42, pending.body, written);
        pending.generation = static_cast<std::uint64_t>(generation);
    } else if (encoded && request.kind == ScriptableOverrideKind::type23) {
        pending.channel = request.channel;
        pending.bitCount = static_cast<std::uint16_t>(auth::kType23BitCount);
        auth::Type23Body body{};
        auth::Type23SequenceGuard composedGuard = candidate.type23;
        for (const PendingScriptableOverride& retained : instance->scriptableAuthEstate) {
            if (!same_client_ref(retained.target, request.target)
                || retained.bitCount != auth::kType23BitCount
                || retained.byteCount != auth::kType23ByteCount
                || !auth::decode_type23_body(std::span(retained.body).first(retained.byteCount),
                                             body)) {
                continue;
            }
            for (std::size_t index = 0; index < body.channels.size(); ++index) {
                composedGuard.last[index] =
                    (std::max)(composedGuard.last[index], body.channels[index].sequence);
            }
            break;
        }
        std::int16_t sequence = 0;
        encoded = auth::next_type23_sequence(composedGuard, request.channel, sequence);
        if (encoded) {
            const std::size_t channel = static_cast<std::size_t>(request.channel);
            body.channels[channel] = {request.value, sequence, request.snap};
            encoded = auth::encode_type23_body(body, pending.body, written);
        }
        pending.sequence = sequence;
    } else if (encoded && request.kind == ScriptableOverrideKind::type31) {
        pending.bitCount = static_cast<std::uint16_t>(auth::kType31BitCount);
        std::uint64_t generation = 0;
        encoded = auth::next_type31_generation(candidate.type31, generation)
                  && auth::encode_type31({generation}, candidate.type31, pending.body, written);
        pending.generation = generation;
    } else if (encoded && request.kind == ScriptableOverrideKind::objectiveReset) {
        pending.bitCount = static_cast<std::uint16_t>(auth::kType3BitCount);
        std::int32_t generation = 0;
        auth::Type3Body body{};
        encoded = auth::next_type3_generation(candidate.type3, generation);
        body.generation = generation;
        encoded = encoded && auth::encode_type3(body, candidate.type3, pending.body, written);
        pending.generation = static_cast<std::uint64_t>(generation);
    } else if (encoded && request.kind == ScriptableOverrideKind::task) {
        pending.bitCount = static_cast<std::uint16_t>(auth::kType38BitCount);
        std::int32_t generation = 0;
        encoded = auth::next_type38_generation(candidate.type38, generation)
                  && auth::encode_type38({generation}, candidate.type38, pending.body, written);
        pending.generation = static_cast<std::uint64_t>(generation);
    } else if (encoded && request.kind == ScriptableOverrideKind::authoredScene) {
        std::size_t bits = 0;
        std::uint32_t generation = 0;
        encoded = next_authored_scene_generation(candidate.authoredSceneGeneration, generation)
                  && scene::encode_authored_scene_auth(
                      generation, request.sceneDependencies, pending.body, written, bits);
        pending.bitCount = static_cast<std::uint16_t>(bits);
        pending.generation = generation;
    } else if (encoded
               && (request.kind == ScriptableOverrideKind::authoredSceneEvent
                   || request.kind == ScriptableOverrideKind::authoredSceneStop)) {
        encoded = false;
        for (const auto& retained : instance->scriptableAuthEstate) {
            if (!same_client_ref(retained.target, request.target)) {
                continue;
            }
            std::size_t bits = 0;
            std::uint32_t generation = 0;
            encoded = scene::update_authored_scene_auth(
                          std::span(retained.body).first(retained.byteCount),
                          retained.bitCount,
                          request.sceneEventKey,
                          request.kind == ScriptableOverrideKind::authoredSceneStop,
                          pending.body,
                          written,
                          bits,
                          generation)
                      && generation == candidate.authoredSceneGeneration;
            pending.bitCount = static_cast<std::uint16_t>(bits);
            pending.generation = generation;
            break;
        }
    } else if (encoded && request.kind == ScriptableOverrideKind::dialogue) {
        pending.bitCount = static_cast<std::uint16_t>(auth::kType53BitCount);
        pending.dialogueCue = request.dialogueCue;
        std::int32_t sequence = 0;
        encoded = auth::next_type53_sequence(candidate.type53, request.dialogueCue, sequence)
                  && auth::encode_type53(
                      {request.dialogueCue, sequence}, candidate.type53, pending.body, written);
        pending.dialogueSequence = sequence;
    } else if (encoded && request.kind == ScriptableOverrideKind::sdkAuth) {
        written = request.authByteCount;
        pending.bitCount = request.authBitCount;
        pending.sdkCompiled = true;
        std::copy_n(request.authBody.begin(), written, pending.body.begin());
    } else {
        encoded = false;
    }
    // A mission API body carries only the root fields it sets, and the native override replaces
    // the whole object. Compose it over the last transported body for the same ClientRef first.
    namespace patching = middleware::bap::activity_message::mission_auth_patch;
    const std::span<const std::byte> patch = std::span(pending.body).first(written);
    patching::Layout layout{};
    const bool rootPatch =
        encoded
        && (request.kind == ScriptableOverrideKind::squad
            || request.kind == ScriptableOverrideKind::sdkAuth)
        && patching::parse(request.target.authSchema, patch, pending.bitCount, layout);
    if (rootPatch) {
        std::span<const std::byte> previous{};
        std::size_t previousBits = 0;
        // A tail never holds the same ClientRef as another pending body, so the predecessor is
        // always in the transported estate.
        for (const auto& retained : instance->scriptableAuthEstate) {
            if (same_client_ref(retained.target, request.target)) {
                previous = std::span(retained.body).first(retained.byteCount);
                previousBits = retained.bitCount;
                break;
            }
        }
        std::size_t composedBits = 0;
        encoded = patching::compose(request.target.authSchema,
                                    previous,
                                    previousBits,
                                    patch,
                                    pending.bitCount,
                                    pending.body,
                                    written,
                                    composedBits);
        if (encoded) {
            pending.bitCount = static_cast<std::uint16_t>(composedBits);
        }
    }
    if (!encoded || written > (std::numeric_limits<std::uint16_t>::max)()) {
        ++g_refusedControls;
    } else {
        if (guard != nullptr && !guard->occupied) {
            // Reserve only the target identity. Mutable lane state commits after transport stages
            // the exact body; type 2 in particular retains a complete per-actor channel set.
            guard->target = request.target;
            guard->occupied = true;
        }
        pending.byteCount = static_cast<std::uint16_t>(written);
        touch(*instance);
        if (joinsTail) {
            instance->pendingScriptableTail[instance->pendingScriptableTailCount] = pending;
            ++instance->pendingScriptableTailCount;
        } else {
            instance->pendingScriptable = pending;
            instance->view.outputPending = true;
            instance->view.outputKind = OutputKind::scriptableOverride;
            instance->view.outputStatus = OutputStatus::pending;
            instance->view.lastOutputAttemptTick = 0;
            instance->view.lastOutputSourceGeneration = 0;
            instance->view.outputAttempts = 0;
        }
        if (!joinsTail) {
            instance->view.scriptableRevision = pending.revision;
        }
        event.kind = EventKind::scriptableOverrideCommitted;
        event.scriptableRevision = pending.revision;
    }
    append_event(event);
    instance->view.lastEventSequence = g_sequence;
}

} // namespace detail

/** Holds one unarmed exact revision while its durable Mission State assignment publishes. */
bool reserve_scriptable_output(const state::activity::SessionBinding& binding,
                               ScriptableOutputReservation& output,
                               std::uint64_t intentSequence) noexcept {
    output = {};
    if (!state::activity::binding_matches(binding)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    const bool available =
        instance != nullptr && instance->view.active && !instance->view.outputPending
        && !instance->view.scriptableReservationPending && !has_queued_control(binding)
        && instance->view.scriptableRevision != (std::numeric_limits<std::uint64_t>::max)()
        && g_scriptableReservationSequence != (std::numeric_limits<std::uint64_t>::max)();
    if (!available) {
        ++g_refusedControls;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    ++g_scriptableReservationSequence;
    output.binding = binding;
    output.resetGeneration = g_scriptableReservationGeneration;
    output.token = g_scriptableReservationSequence;
    output.revision = instance->view.scriptableRevision + 1;
    output.intentSequence = intentSequence;
    instance->scriptableReservation = output;
    instance->view.scriptableReservedRevision = output.revision;
    instance->view.scriptableReservationPending = true;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Releases one exact unarmed reservation without changing the Host output revision. */
bool release_scriptable_output(const ScriptableOutputReservation& reservation) noexcept {
    if (reservation.resetGeneration == 0 || reservation.token == 0 || reservation.revision == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(reservation.binding);
    const bool owned = instance != nullptr && instance->view.scriptableReservationPending
                       && same_reservation(instance->scriptableReservation, reservation);
    if (owned) {
        clear_reservation(*instance);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return owned;
}

/** Atomically withdraws one queued durable reducer row or reports its committed disposition. */
ScriptableWithdrawStatus withdraw_scriptable_output(const state::activity::SessionBinding& binding,
                                                    std::uint64_t intentSequence,
                                                    std::uint64_t expectedRevision) noexcept {
    if (intentSequence == 0 || expectedRevision == 0) {
        return ScriptableWithdrawStatus::mismatch;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    if (instance == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return ScriptableWithdrawStatus::mismatch;
    }
    if (instance->view.scriptableTransportRevision == expectedRevision) {
        ReleaseSRWLockExclusive(&g_lock);
        return ScriptableWithdrawStatus::transportStaged;
    }
    if (instance->view.scriptableRevision > expectedRevision
        || instance->view.scriptableTransportRevision > expectedRevision) {
        ReleaseSRWLockExclusive(&g_lock);
        return ScriptableWithdrawStatus::advanced;
    }
    if (instance->view.scriptableRevision == expectedRevision) {
        const ScriptableWithdrawStatus status =
            instance->view.outputPending
                    && instance->view.outputKind == OutputKind::scriptableOverride
                ? ScriptableWithdrawStatus::committed
                : ScriptableWithdrawStatus::canceled;
        ReleaseSRWLockExclusive(&g_lock);
        return status;
    }
    if (instance->view.outputPending || instance->view.scriptableReservationPending) {
        ReleaseSRWLockExclusive(&g_lock);
        return ScriptableWithdrawStatus::mismatch;
    }
    for (std::size_t index = g_pendingRead; index < g_pending.size(); ++index) {
        PendingInput& pending = g_pending[index];
        if (pending.kind != PendingKind::scriptableControl
            || !same_binding(pending.scriptableControl.binding, binding)
            || pending.scriptableControl.expectedIntentSequence != intentSequence
            || pending.scriptableControl.expectedRevision != expectedRevision) {
            continue;
        }
        pending.scriptableControl = {};
        pending.kind = PendingKind::discardedControl;
        --g_queuedControls;
        ReleaseSRWLockExclusive(&g_lock);
        return ScriptableWithdrawStatus::withdrawn;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ScriptableWithdrawStatus::absent;
}

} // namespace sunrise::server::activity::host
