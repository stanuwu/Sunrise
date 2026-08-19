#include <limits>
#include <variant>

#include "command_validation_state.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

[[nodiscard]] bool same_blueprint(world::BlueprintRef left, world::BlueprintRef right) noexcept {
    return left.id == right.id && left.version == right.version;
}

[[nodiscard]] bool
checked_add(std::int64_t left, std::int64_t right, std::int64_t& result) noexcept {
    if ((right > 0 && left > (std::numeric_limits<std::int64_t>::max)() - right)
        || (right < 0 && left < (std::numeric_limits<std::int64_t>::min)() - right)) {
        return false;
    }
    result = left + right;
    return true;
}

} // namespace

/** Checks an exact versioned attack and its policy-owned damage amount. */
bool registered_attack_matches(const BubbleHost::Storage::WorldSlot& slot,
                               const world::SubmitDamageCommand& damage,
                               world::PolicyOwnerId owner) noexcept {
    for (const BubbleHost::Storage::AttackProfileSlot& candidate : slot.attackProfiles) {
        if (candidate.occupied && candidate.profile.attack.id == damage.attack.id
            && candidate.profile.attack.version == damage.attack.version
            && candidate.profile.policyOwnerId == owner
            && candidate.profile.damage == damage.amount) {
            return true;
        }
    }
    return false;
}

/** Checks combat binding capacity and actor-profile replacement rules. */
bool valid_combat_configuration(const BubbleHost::Storage::WorldSlot& slot,
                                const world::HostCommand& command) noexcept {
    const auto& value = std::get<world::ConfigureCombatCommand>(command.payload);
    bool hasFree = false;
    for (const BubbleHost::Storage::CombatantBinding& binding : slot.combatants) {
        if (binding.occupied && world::equal_actor(binding.actor, value.actor)) {
            return binding.profile.id == 0 || same_blueprint(binding.profile, value.combatProfile);
        }
        hasFree = hasFree || !binding.occupied;
    }
    return hasFree;
}

/** Checks exact trigger identity and tracked backend capacity before mutation. */
bool valid_trigger_effect(const BubbleHost::Storage::WorldSlot& slot,
                          const world::HostCommand& command) noexcept {
    const mechanics::TriggerSystemSnapshot snapshot = slot.triggers.snapshot();
    if (world::command_kind(command) == world::HostCommandKind::removeTrigger) {
        const auto& value = std::get<world::RemoveTriggerCommand>(command.payload);
        bool logical = false;
        for (const mechanics::TriggerSlotSnapshot& trigger : snapshot.slots) {
            logical = logical
                      || (trigger.active && trigger.triggerId == value.triggerId
                          && trigger.generation == value.triggerGeneration
                          && trigger.policyOwnerId == command.header.policyOwnerId);
        }
        bool backend = false;
        for (const BubbleHost::Storage::TriggerBinding& binding : slot.triggerBindings) {
            backend = backend
                      || (binding.occupied && binding.triggerId == value.triggerId
                          && binding.trigger.generation == value.triggerGeneration);
        }
        return logical && backend;
    }

    const auto& value = std::get<world::CreateTriggerCommand>(command.payload);
    std::size_t bodyCount = 0;
    bool hasLogicalSlot = false;
    for (const BubbleHost::Storage::BodyBinding& binding : slot.bodies) {
        bodyCount += binding.occupied ? 1U : 0U;
    }
    for (const BubbleHost::Storage::TriggerBinding& binding : slot.triggerBindings) {
        if (binding.occupied && binding.triggerId == value.triggerId) {
            return false;
        }
        bodyCount += binding.occupied ? 1U : 0U;
    }
    for (const mechanics::TriggerSlotSnapshot& trigger : snapshot.slots) {
        if (trigger.active && trigger.triggerId == value.triggerId
            && trigger.policyOwnerId == command.header.policyOwnerId) {
            return false;
        }
        hasLogicalSlot = hasLogicalSlot
                         || (!trigger.active
                             && trigger.generation != (std::numeric_limits<std::uint64_t>::max)());
    }
    return hasLogicalSlot && bodyCount < backend::kBodyCapacity;
}

/** Checks exact objective revision, arithmetic, bounds, and monotonic policy. */
bool valid_objective_effect(const BubbleHost::Storage::WorldSlot& slot,
                            const world::HostCommand& command) noexcept {
    std::uint64_t counterId = 0;
    std::uint64_t expectedRevision = 0;
    bool add = false;
    std::int64_t requested = 0;
    if (world::command_kind(command) == world::HostCommandKind::addObjectiveCounter) {
        const auto& value = std::get<world::AddObjectiveCounterCommand>(command.payload);
        counterId = value.counterId;
        expectedRevision = value.expectedRevision;
        requested = value.amount;
        add = true;
    } else {
        const auto& value = std::get<world::SetObjectiveCounterCommand>(command.payload);
        counterId = value.counterId;
        expectedRevision = value.expectedRevision;
        requested = value.value;
    }
    const mechanics::ObjectiveLedgerSnapshot snapshot = slot.objectives.snapshot();
    for (const mechanics::ObjectiveCounterSnapshot& counter : snapshot.counters) {
        if (!counter.active || counter.definition.counterId != counterId) {
            continue;
        }
        std::int64_t next = requested;
        if (counter.definition.policyOwnerId != command.header.policyOwnerId
            || counter.revision != expectedRevision
            || (add && !checked_add(counter.value, requested, next))) {
            return false;
        }
        return next >= counter.definition.minimumValue && next <= counter.definition.maximumValue
               && (counter.definition.policy != mechanics::ObjectivePolicy::monotonic
                   || next >= counter.value)
               && (next == counter.value
                   || counter.revision != (std::numeric_limits<std::uint64_t>::max)());
    }
    return false;
}

/** Checks participant ownership, exact channel membership, and revision capacity. */
bool valid_credit_effect(const BubbleHost::Storage::WorldSlot& slot,
                         const world::HostCommand& command) noexcept {
    const auto& value = std::get<world::RecordParticipantCreditCommand>(command.payload);
    const mechanics::ParticipantCreditLedgerSnapshot snapshot = slot.credits.snapshot();
    for (const mechanics::ParticipantCreditSnapshot& participant : snapshot.participants) {
        if (!participant.active || participant.definition.participantId != value.participantId) {
            continue;
        }
        if (participant.definition.policyOwnerId != command.header.policyOwnerId
            || participant.revision == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        for (std::size_t index = 0; index < participant.definition.channelCount; ++index) {
            if (participant.channels[index].channelId == value.channelId) {
                return true;
            }
        }
        return false;
    }
    return false;
}

/** Checks that a timer slot can advance its generation before scheduling. */
bool valid_timer_effect(const BubbleHost::Storage::WorldSlot& slot) noexcept {
    const mechanics::TickTimerServiceSnapshot snapshot = slot.timers.snapshot();
    for (const mechanics::TickTimerSlotSnapshot& timer : snapshot.slots) {
        if (!timer.active && timer.generation != (std::numeric_limits<std::uint64_t>::max)()) {
            return true;
        }
    }
    return false;
}

} // namespace sunrise::server::gameplay::physics::host::detail
