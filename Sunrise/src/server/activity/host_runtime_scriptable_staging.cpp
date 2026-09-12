#include <algorithm>
#include <limits>

#include "../../state/activity/runtime.h"
#include "../gameplay/squad_entity_retirement.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace {

namespace auth = middleware::bap::activity_message::scriptable_auth;
namespace squad = middleware::bap::activity_message::squad_auth;
using namespace detail;

} // namespace

/** Reads the one pending typed ClientRef body without changing its counter. */
bool pending_scriptable_override(const state::activity::SessionBinding& binding,
                                 PendingScriptableOverride& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    const bool pending = instance != nullptr && instance->view.active
                         && instance->view.outputPending
                         && instance->view.outputKind == OutputKind::scriptableOverride
                         && instance->pendingScriptable.revision != 0;
    if (pending) {
        output = instance->pendingScriptable;
    }
    ReleaseSRWLockShared(&g_lock);
    return pending;
}

/** Reads one pending body only for the ActivityClient generation that authorized it. */
bool pending_scriptable_override_for_activity_client(const state::activity::SessionBinding& binding,
                                                     std::uint64_t activityClientGeneration,
                                                     PendingScriptableOverride& output) noexcept {
    output = {};
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    bool pending = instance != nullptr && instance->view.active && instance->view.outputPending
                   && instance->view.outputKind == OutputKind::scriptableOverride
                   && instance->pendingScriptable.revision != 0;
    if (pending && instance->pendingScriptable.expectedActivityClientGeneration != 0
        && instance->pendingScriptable.expectedActivityClientGeneration
               != activityClientGeneration) {
        const std::uint64_t revision = instance->pendingScriptable.revision;
        cancel_pending(*instance, binding, revision);
        pending = false;
    }
    if (pending) {
        output = instance->pendingScriptable;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return pending;
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
    if (pending.revision == 0 || status == OutputStatus::idle || status == OutputStatus::pending
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
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::squad
               && pending.generation <= squad::kMaximumGeneration) {
        std::uint32_t next = 0;
        nextCounter = squad::next_generation(guard->squad, next) && next == pending.generation;
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
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::combatantSequence) {
        // A retained generic Auth program may be ahead of this typed guard.
        nextCounter = pending.generation > guard->type2AtomGeneration
                      && pending.generation <= squad::kMaximumGeneration;
    } else if (guard != nullptr && pending.kind == ScriptableOverrideKind::object) {
        std::int32_t next = 0;
        nextCounter = auth::next_type4_generation(guard->type4, next)
                      && static_cast<std::uint64_t>(next) == pending.generation;
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
        std::uint64_t next = 0;
        nextCounter =
            auth::next_type31_generation(guard->type31, next) && next == pending.generation;
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
    } else if (pending.kind == ScriptableOverrideKind::combatantSequence) {
        guard->type2AtomGeneration = static_cast<std::uint32_t>(pending.generation);
    } else if (pending.kind == ScriptableOverrideKind::object) {
        guard->type4.last = static_cast<std::int32_t>(pending.generation);
        guard->type4.hasLast = true;
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
    } else if (pending.kind == ScriptableOverrideKind::dialogue) {
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

/** @return True when any instance still owes a Host output. */
bool any_output_pending() noexcept {
    AcquireSRWLockShared(&g_lock);
    bool pending = false;
    for (const Instance& instance : g_instances) {
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
    for (const Instance& candidate : g_instances) {
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

} // namespace sunrise::server::activity::host
