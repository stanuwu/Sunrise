#include <algorithm>
#include <limits>

#include "../../state/activity/mission/runtime.h"
#include "../../state/activity/runtime.h"
#include "../gameplay/squad_entity_retirement.h"
#include "host_runtime_ghost_link.h"
#include "host_runtime_internal.h"
#include "host_scriptable_owner.h"

namespace sunrise::server::activity::host {
namespace {

namespace auth = middleware::bap::activity_message::scriptable_auth;
namespace squad = middleware::bap::activity_message::squad_auth;
using namespace detail;

/**
 * A borrowed output excludes every input already admitted before its transport attempt.
 * @param binding Exact activity owning the output.
 * @param output Receives both accepted-input heads while the Host lock is held.
 */
void stamp_output_boundary(const state::activity::SessionBinding& binding,
                           PendingScriptableOverride& output) noexcept {
    state::activity::mission::InputSequenceSnapshot input{};
    output.missionInputBoundaryKnown =
        state::activity::mission::input_sequence_snapshot(binding, input);
    output.missionInputSequenceAtStage = input.issued;
    output.clientMessageSequenceAtStage = latest_client_message_sequence();
}

} // namespace

/** The Host lock orders the framing head with the durable accepted-input head. */
bool publication_input_boundary(const state::activity::SessionBinding& binding,
                                std::uint64_t& attemptGeneration,
                                std::uint64_t& inputSequence,
                                std::uint64_t& clientMessageSequence) noexcept {
    attemptGeneration = inputSequence = clientMessageSequence = 0;
    AcquireSRWLockShared(&g_lock);
    state::activity::mission::InputSequenceSnapshot input{};
    const bool known = state::activity::mission::input_sequence_snapshot(binding, input);
    if (known) {
        attemptGeneration = input.attemptGeneration;
        inputSequence = input.issued;
        clientMessageSequence = latest_client_message_sequence();
    }
    ReleaseSRWLockShared(&g_lock);
    return known;
}

/** Reads one pending body only for the ActivityClient generation that authorized it. */
bool pending_scriptable_override_for_activity_client(const state::activity::SessionBinding& binding,
                                                     std::uint64_t activityClientGeneration,
                                                     PendingScriptableOverride& output) noexcept {
    output = {};
    if (activityClientGeneration == 0) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    const bool pending = ownership::readable(instance, activityClientGeneration);
    if (pending) {
        output = instance->pendingScriptable;
        stamp_output_boundary(binding, output);
    }
    ReleaseSRWLockShared(&g_lock);
    return pending;
}

void retire_scriptable_client(const state::activity::SessionBinding& binding,
                              std::uint64_t generation) noexcept {
    if (generation == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    if (ownership::owns(instance, generation)) {
        cancel_pending(*instance, binding, instance->pendingScriptable.revision);
    }
    g_queuedControls -=
        ownership::retire(std::span(g_pending).subspan(g_pendingRead), binding, generation);
    ReleaseSRWLockExclusive(&g_lock);
}

/** Cancels one exact unstaged typed override revision without advancing its slot counter. */
bool cancel_pending_scriptable_override(const state::activity::SessionBinding& binding,
                                        std::uint64_t expectedRevision) noexcept {
    if (expectedRevision == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    const bool canceled = instance != nullptr && instance->view.active
                          && instance->view.outputPending
                          && instance->view.outputKind == OutputKind::scriptableOverride
                          && instance->pendingScriptable.revision == expectedRevision;
    if (canceled) {
        cancel_pending(*instance, binding, expectedRevision);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return canceled;
}

/** Records one refused typed-body attempt without consuming its sequence or generation. */
void note_scriptable_attempt(const state::activity::SessionBinding& binding,
                             std::uint64_t sourceGeneration,
                             const PendingScriptableOverride& pending,
                             OutputStatus status) noexcept {
    if (pending.revision == 0
        || (pending.expectedActivityClientGeneration != 0
            && pending.expectedActivityClientGeneration != sourceGeneration)
        || status == OutputStatus::idle || status == OutputStatus::pending
        || status == OutputStatus::transportStaged || status == OutputStatus::canceled) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    if (instance != nullptr && instance->view.outputPending
        && instance->view.outputKind == OutputKind::scriptableOverride
        && same_pending(instance->pendingScriptable, pending)) {
        instance->view.lastOutputAttemptTick = GetTickCount64();
        instance->view.lastOutputSourceGeneration = sourceGeneration;
        ++instance->view.outputAttempts;
        instance->view.outputStatus = status;
        const bool terminal = status == OutputStatus::noLayout || status == OutputStatus::noGroups
                              || status == OutputStatus::noOverrideTarget
                              || status == OutputStatus::ambiguousLinks
                              || status == OutputStatus::frameRefused;
        if (terminal) {
            const std::uint64_t revision = instance->pendingScriptable.revision;
            cancel_pending(*instance, binding, revision);
            instance->view.outputStatus = status;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** @return True when this body carries the exact next counter its committed guard expects. */
[[nodiscard]] bool staged_counter_matches(const ScriptableGuard* guard,
                                          const PendingScriptableOverride& pending) noexcept {
    bool nextCounter = false;
    // Lifetime and a compiled SDK Auth carry no counter of their own.
    if (pending.kind == ScriptableOverrideKind::lifetime
        || (guard != nullptr && pending.kind == ScriptableOverrideKind::sdkAuth
            && pending.sdkCompiled)) {
        nextCounter = true;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::squadObjective) {
        // The revision was derived from the transported estate while this ClientRef was reserved.
        nextCounter = pending.generation > 0 && pending.generation <= squad::kMaximumGeneration;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::squad
               && pending.generation <= squad::kMaximumGeneration) {
        nextCounter = pending.generation > (guard->squad.hasLast ? guard->squad.last : 0U);
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::combatantChannel) {
        auth::Type2ChannelState candidate = guard->type2;
        std::uint32_t revision = 0;
        nextCounter =
            auth::next_type2_revision(candidate, revision) && revision == pending.generation;
        candidate.revision = revision;
        nextCounter =
            nextCounter
            && auth::set_type2_channel(candidate, pending.channelHash, pending.channelValue);
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::combatantBinding) {
        auth::Type2ChannelState candidate = guard->type2;
        std::uint32_t revision = 0;
        nextCounter =
            auth::next_type2_revision(candidate, revision) && revision == pending.generation;
        candidate.revision = revision;
        candidate.actorBinding = auth::Type2ActorBinding::squadMember;
    } else if (guard != nullptr
               && (pending.kind == ScriptableOverrideKind::combatantProgram
                   || pending.kind == ScriptableOverrideKind::combatantRetirement)) {
        nextCounter = pending.generation > guard->type2AtomGeneration
                      && pending.generation <= squad::kMaximumGeneration
                      && (pending.actorSpawnGeneration == 0
                          || (pending.actorSpawnGeneration > guard->type2SpawnGeneration
                              && pending.actorSpawnGeneration <= squad::kMaximumGeneration));
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::combatantSequence) {
        // A retained generic Auth program may be ahead of this typed guard.
        nextCounter = pending.generation > guard->type2AtomGeneration
                      && pending.generation <= squad::kMaximumGeneration;
    } else if (guard != nullptr
               && (pending.kind == ScriptableOverrideKind::object
                   || pending.kind == ScriptableOverrideKind::interactableObject)) {
        nextCounter =
            pending.generation > static_cast<std::uint64_t>((std::max)(guard->type4.last, 0))
            && pending.generation <= squad::kMaximumGeneration;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::damageWatch) {
        nextCounter = pending.generation > guard->damageRevision
                      && pending.generation <= squad::kMaximumGeneration;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::ghostLink) {
        nextCounter = pending.generation > guard->ghostLink.generation
                      && pending.generation <= squad::kMaximumGeneration;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::sequence) {
        std::uint8_t next = 0;
        nextCounter = auth::next_type5_revision(guard->type5, next) && next == pending.generation;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::cinematic) {
        std::uint32_t next = 0;
        nextCounter = auth::next_type6_generation(guard->type6, next) && next == pending.generation;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::performance) {
        std::int32_t next = 0;
        nextCounter = auth::next_type42_generation(guard->type42, next)
                      && static_cast<std::uint64_t>(next) == pending.generation;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::type23) {
        std::int16_t next = 0;
        nextCounter = auth::next_type23_sequence(guard->type23, pending.channel, next)
                      && next == pending.sequence;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::type31) {
        nextCounter = !guard->type31.hasLast || pending.generation >= guard->type31.last;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::objectiveReset) {
        std::int32_t next = 0;
        nextCounter = auth::next_type3_generation(guard->type3, next)
                      && static_cast<std::uint64_t>(next) == pending.generation;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::task) {
        std::int32_t next = 0;
        nextCounter = auth::next_type38_generation(guard->type38, next)
                      && static_cast<std::uint64_t>(next) == pending.generation;
    } else if (guard != nullptr
               && (pending.kind == ScriptableOverrideKind::authoredSceneEvent
                   || pending.kind == ScriptableOverrideKind::authoredSceneStop)) {
        nextCounter =
            pending.generation != 0 && pending.generation == guard->authoredSceneGeneration;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::authoredScene) {
        std::uint32_t next = 0;
        nextCounter = next_authored_scene_generation(guard->authoredSceneGeneration, next)
                      && next == pending.generation;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::dialogue) {
        std::int32_t next = 0;
        nextCounter = auth::next_type53_sequence(guard->type53, pending.dialogueCue, next)
                      && next == pending.dialogueSequence;
    }
    return nextCounter;
}

/** Advances one committed guard to the body that has just reached transport. */
void advance_staged_guard(ScriptableGuard* guard,
                          const PendingScriptableOverride& pending) noexcept {
    if (guard == nullptr) {
        return;
    }
    if (pending.kind == ScriptableOverrideKind::squad) {
        guard->squad.last = static_cast<std::uint32_t>(pending.generation);
        guard->squad.hasLast = true;
    } else if (pending.kind == ScriptableOverrideKind::combatantChannel) {
        guard->type2.revision = static_cast<std::uint32_t>(pending.generation);
        static_cast<void>(
            auth::set_type2_channel(guard->type2, pending.channelHash, pending.channelValue));
    } else if (pending.kind == ScriptableOverrideKind::combatantBinding) {
        guard->type2.revision = static_cast<std::uint32_t>(pending.generation);
        guard->type2.actorBinding = auth::Type2ActorBinding::squadMember;
    } else if (pending.kind == ScriptableOverrideKind::combatantProgram
               || pending.kind == ScriptableOverrideKind::combatantRetirement) {
        guard->type2AtomGeneration = static_cast<std::uint32_t>(pending.generation);
        if (pending.actorSpawnGeneration != 0) {
            guard->type2SpawnGeneration = pending.actorSpawnGeneration;
        }
    } else if (pending.kind == ScriptableOverrideKind::combatantSequence) {
        guard->type2AtomGeneration = static_cast<std::uint32_t>(pending.generation);
    } else if (pending.kind == ScriptableOverrideKind::object
               || pending.kind == ScriptableOverrideKind::interactableObject) {
        guard->type4.last = static_cast<std::int32_t>(pending.generation);
        guard->type4.hasLast = true;
    } else if (pending.kind == ScriptableOverrideKind::damageWatch) {
        guard->damageRevision = static_cast<std::uint32_t>(pending.generation);
    } else if (pending.kind == ScriptableOverrideKind::ghostLink) {
        ghost_link::advance(*guard, pending);
    } else if (pending.kind == ScriptableOverrideKind::sequence) {
        guard->type5.last = static_cast<std::uint8_t>(pending.generation);
        guard->type5.hasLast = true;
    } else if (pending.kind == ScriptableOverrideKind::cinematic) {
        guard->type6.last = static_cast<std::uint32_t>(pending.generation);
        guard->type6.hasLast = true;
    } else if (pending.kind == ScriptableOverrideKind::performance) {
        guard->type42.last = static_cast<std::int32_t>(pending.generation);
        guard->type42.hasLast = true;
    } else if (pending.kind == ScriptableOverrideKind::type23) {
        guard->type23.last[static_cast<std::size_t>(pending.channel)] = pending.sequence;
    } else if (pending.kind == ScriptableOverrideKind::type31) {
        guard->type31.last = pending.generation;
        guard->type31.hasLast = true;
    } else if (pending.kind == ScriptableOverrideKind::objectiveReset) {
        guard->type3.last = static_cast<std::int32_t>(pending.generation);
        guard->type3.hasLast = true;
    } else if (pending.kind == ScriptableOverrideKind::task) {
        guard->type38.last = static_cast<std::int32_t>(pending.generation);
        guard->type38.hasLast = true;
    } else if (pending.kind == ScriptableOverrideKind::authoredScene) {
        guard->authoredSceneGeneration = static_cast<std::uint32_t>(pending.generation);
    } else if (pending.kind == ScriptableOverrideKind::dialogue
               && pending.dialogueCue < guard->type53.last.size()) {
        auto& last = guard->type53.last[pending.dialogueCue];
        last = (std::max)(last, pending.dialogueSequence);
    }
}

/** Records that one pending body reached the transport, so its retained estate can advance. */
void note_scriptable_transport_staged(const state::activity::SessionBinding& binding,
                                      std::uint64_t sourceGeneration,
                                      const PendingScriptableOverride& pending) noexcept {
    if (pending.revision == 0
        || (pending.expectedActivityClientGeneration != 0
            && pending.expectedActivityClientGeneration != sourceGeneration)) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    ScriptableGuard* guard = instance != nullptr ? find_guard(*instance, pending.target) : nullptr;
    const bool nextCounter = staged_counter_matches(guard, pending);
    if (instance != nullptr && nextCounter && instance->view.outputPending
        && instance->view.outputKind == OutputKind::scriptableOverride
        && same_pending(instance->pendingScriptable, pending)) {
        const bool retained = retain_scriptable_auth(*instance, pending, sourceGeneration);
        if (!retained) {
            ++g_refusedControls;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        server::gameplay::squad_entity_retirement::record_delivered_target(
            binding, sourceGeneration, pending);
        advance_staged_guard(guard, pending);
        if (pending.kind == ScriptableOverrideKind::lifetime) {
            // Latch the state so every later msg 5 keeps reporting it.
            instance->view.lifetimeState = pending.lifetimeState;
        }
        instance->view.lastOutputAttemptTick = GetTickCount64();
        Event event{};
        event.binding = binding;
        event.tick = instance->view.lastOutputAttemptTick;
        event.kind = EventKind::scriptableOverrideTransportStaged;
        event.sourceGeneration = sourceGeneration;
        // The tail rode out on this same body, so it stages with the head or not at all.
        for (std::size_t index = 0; index < instance->pendingScriptableTailCount; ++index) {
            const PendingScriptableOverride& queued = instance->pendingScriptableTail[index];
            ScriptableGuard* const queuedGuard = find_guard(*instance, queued.target);
            if (!staged_counter_matches(queuedGuard, queued)
                || !retain_scriptable_auth(*instance, queued, sourceGeneration)) {
                ++g_refusedControls;
                continue;
            }
            server::gameplay::squad_entity_retirement::record_delivered_target(
                binding, sourceGeneration, queued);
            advance_staged_guard(queuedGuard, queued);
            event.scriptableRevision = queued.revision;
            append_event(event);
        }
        instance->pendingScriptableTail.fill({});
        instance->pendingScriptableTailCount = 0;
        instance->view.scriptableTransportRevision = instance->view.scriptableRevision;
        instance->view.lastOutputSourceGeneration = sourceGeneration;
        ++instance->view.outputAttempts;
        instance->view.outputStatus = OutputStatus::transportStaged;
        instance->view.outputPending = false;
        instance->view.outputKind = OutputKind::none;
        instance->pendingScriptable = {};
        event.scriptableRevision = pending.revision;
        append_event(event);
        instance->view.lastEventSequence = g_sequence;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies the pending overrides that have no output yet. @return How many were written. */
std::size_t pending_scriptable_tail(const state::activity::SessionBinding& binding,
                                    std::span<PendingScriptableOverride> output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    std::size_t written = 0;
    if (instance != nullptr && instance->view.active && instance->view.outputPending
        && instance->view.outputKind == OutputKind::scriptableOverride) {
        const std::size_t count = (std::min)(instance->pendingScriptableTailCount, output.size());
        for (; written < count; ++written) {
            output[written] = instance->pendingScriptableTail[written];
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return written;
}

/**
 * Copies one retained output under the same lock that admits input and stages output.
 * @param binding Exact activity owning the output.
 * @param revision Exact transported output revision.
 * @param output Receives the retained body, or an empty value when absent.
 * @return True when that output remains retained.
 */
bool staged_scriptable_override(const state::activity::SessionBinding& binding,
                                std::uint64_t revision,
                                PendingScriptableOverride& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Instance* instance = find_instance(binding);
    bool found = false;
    if (instance != nullptr) {
        for (const auto& retained : instance->scriptableAuthEstate) {
            if (retained.revision == revision) {
                output = retained;
                found = true;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/** @return True when any instance still owes a Host output. */
bool any_output_pending() noexcept {
    AcquireSRWLockShared(&g_lock);
    bool pending = false;
    for (const auto& owned : g_instances) {
        if (!owned) {
            continue;
        }
        const Instance& instance = *owned;
        pending =
            pending
            || (instance.occupied && instance.view.active
                && (instance.view.outputPending || has_queued_control(instance.view.binding)));
    }
    ReleaseSRWLockShared(&g_lock);
    return pending;
}

/** Copies the retained Auth estate for one exact ActivityClient generation. */
bool scriptable_auth_estate(const state::activity::SessionBinding& binding,
                            std::uint64_t activityClientGeneration,
                            std::vector<PendingScriptableOverride>& output) noexcept {
    output.clear();
    if (activityClientGeneration == 0) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    const Instance* instance = nullptr;
    for (const auto& owned : g_instances) {
        if (!owned) {
            continue;
        }
        const Instance& candidate = *owned;
        if (candidate.occupied && same_binding(candidate.view.binding, binding)) {
            instance = &candidate;
            break;
        }
    }
    bool copied = true;
    if (instance != nullptr && instance->view.active) {
        try {
            output.reserve(instance->scriptableAuthEstate.size());
            for (const PendingScriptableOverride& retained : instance->scriptableAuthEstate) {
                // The ActivityClient generation is a transport revision that advances on ordinary
                // region advertisements, and the SessionBinding already owns estate lifetime.
                // Filtering retained mission state by it erases every non-squad Auth lane.
                output.push_back(retained);
            }
        } catch (const std::bad_alloc&) {
            output.clear();
            copied = false;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return copied;
}

namespace detail {

/** Encodes one type-31 arm or disarm. */
bool encode_trigger_pulse(const ScriptableRequest& request,
                          const auth::Type31GenerationGuard& guard,
                          PendingScriptableOverride& pending,
                          std::size_t& written) noexcept {
    written = 0;
    pending.bitCount = static_cast<std::uint16_t>(auth::kType31BitCount);
    std::uint64_t generation = 0;
    const bool encoded =
        auth::type31_arm_generation(guard, generation)
        && auth::encode_type31({generation, request.triggerEnabled}, guard, pending.body, written);
    pending.generation = generation;
    return encoded;
}

/** Encodes one dialogue pulse on top of the body last transported for its slot. */
bool encode_dialogue_pulse(const Instance& instance,
                           const ScriptableRequest& request,
                           const auth::Type53SequenceGuard& guard,
                           PendingScriptableOverride& pending,
                           std::size_t& written) noexcept {
    written = 0;
    pending.dialogueCue = request.dialogueCue;
    auth::Type53Body base{};
    for (const PendingScriptableOverride& retained : instance.scriptableAuthEstate) {
        const ScriptableTarget& target = retained.target;
        if (target.objectTag == request.target.objectTag
            && target.registryKey == request.target.registryKey
            && target.slotIndex == request.target.slotIndex
            && target.slotType == request.target.slotType) {
            // A body in another form carries no waiting row to keep.
            if (!auth::decode_type53_body(
                    std::span(retained.body).first(retained.byteCount), retained.bitCount, base)) {
                base = {};
            }
            break;
        }
    }
    std::int32_t sequence = 0;
    std::size_t bits = 0;
    auth::Type53Body body{};
    const bool encoded =
        auth::next_type53_sequence(guard, request.dialogueCue, sequence)
        && auth::compose_type53(
            base, {request.dialogueCue, sequence, request.dialogueFilter}, guard, body)
        && auth::encode_type53_body(body, pending.body, written, bits);
    pending.bitCount = static_cast<std::uint16_t>(bits);
    pending.dialogueSequence = sequence;
    return encoded;
}

} // namespace detail

} // namespace sunrise::server::activity::host
