#include "common_sync.h"

#include <cstdint>
#include <limits>

namespace sunrise::server::gameplay::physics::common {
namespace {

[[nodiscard]] bool valid_binding(const Binding& binding) noexcept {
    return binding.activitySessionId != 0 && binding.bindingGeneration != 0;
}

[[nodiscard]] bool equal_binding(const Binding& left, const Binding& right) noexcept {
    return left.patchEpoch == right.patchEpoch && left.activitySessionId == right.activitySessionId
           && left.bindingGeneration == right.bindingGeneration;
}

[[nodiscard]] bool matches(const Binding& binding,
                           const middleware::gameplay::external::CommonState& common) noexcept {
    return common.entryCount == 1 && common.patchEpoch == binding.patchEpoch
           && common.entries[0].activitySessionId == binding.activitySessionId;
}

/** Invalidates confirmed coverage after an unrequested generation is observed. */
ObserveResult invalidate_observation(State& state, ObserveResult result) noexcept {
    state.pendingContributionGeneration = 0;
    state.pendingPacketGeneration = 0;
    state.observedGeneration = 0;
    state.targetGeneration = 0;
    state.phase = Phase::awaitingObservation;
    state.commonPending = false;
    state.commonCovered = false;
    return result;
}

} // namespace

/** Clears a common synchronization lifetime. */
void reset(State& state) noexcept {
    const std::uint64_t generation = state.generation;
    const std::uint64_t contributionGeneration = state.nextContributionGeneration;
    state = {};
    state.generation = generation;
    state.nextContributionGeneration = contributionGeneration;
}

/** Binds a fresh ActivityClient lifetime and waits for its first common root. */
bool bind(State& state, const Binding& binding) noexcept {
    if (!valid_binding(binding)
        || state.generation == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    const std::uint64_t next = state.generation + 1U;
    const std::uint64_t contributionGeneration = state.nextContributionGeneration;
    state = {};
    state.binding = binding;
    state.generation = next == 0 ? 1 : next;
    state.nextContributionGeneration = contributionGeneration;
    state.phase = Phase::awaitingObservation;
    return true;
}

/** Applies one exact inbound common root without accepting a different activity. */
ObserveResult observe(State& state,
                      const middleware::gameplay::external::CommonState& common) noexcept {
    if (state.phase == Phase::absent || common.entryCount != 1) {
        return state.phase == Phase::absent
                   ? ObserveResult::malformed
                   : invalidate_observation(state, ObserveResult::malformed);
    }
    if (!matches(state.binding, common)) {
        return invalidate_observation(state, ObserveResult::wrongBinding);
    }
    const std::uint8_t generation = common.entries[0].reconciliationGeneration;
    if (state.phase == Phase::awaitingObservation) {
        state.observedGeneration = generation;
        state.targetGeneration = generation;
        state.phase = Phase::observed;
        return ObserveResult::initial;
    }
    if (state.phase == Phase::observed) {
        return generation == state.observedGeneration
                   ? ObserveResult::unchanged
                   : invalidate_observation(state, ObserveResult::unexpectedGeneration);
    }
    if (state.phase == Phase::advanceQueued) {
        if (generation == state.observedGeneration) {
            return ObserveResult::unchanged;
        }
        if (generation == state.targetGeneration) {
            state.observedGeneration = generation;
            state.phase = Phase::confirmed;
            return ObserveResult::confirmed;
        }
        return invalidate_observation(state, ObserveResult::unexpectedGeneration);
    }
    return generation == state.targetGeneration
               ? ObserveResult::unchanged
               : invalidate_observation(state, ObserveResult::unexpectedGeneration);
}

/** Prepares the only allowed next-generation transition without changing state. */
bool prepare_advance(const State& state, AdvancePlan& plan) noexcept {
    plan = {};
    if (state.phase != Phase::observed || !valid_binding(state.binding) || state.generation == 0) {
        return false;
    }
    plan.binding = state.binding;
    plan.stateGeneration = state.generation;
    plan.previousGeneration = state.observedGeneration;
    plan.nextGeneration = static_cast<std::uint8_t>(state.observedGeneration + 1U);
    plan.prepared = true;
    return true;
}

/** Marks a prepared activity message 44 as queued for the bound ActivityClient. */
bool commit_advance(State& state, const AdvancePlan& plan) noexcept {
    if (!plan.prepared || state.phase != Phase::observed || state.generation != plan.stateGeneration
        || !equal_binding(state.binding, plan.binding)
        || state.observedGeneration != plan.previousGeneration
        || plan.nextGeneration != static_cast<std::uint8_t>(plan.previousGeneration + 1U)) {
        return false;
    }
    state.targetGeneration = plan.nextGeneration;
    state.phase = Phase::advanceQueued;
    return true;
}

/** Builds the confirmed count-one common root for an external contribution. */
bool confirmed_common(const State& state,
                      middleware::gameplay::external::CommonState& common) noexcept {
    common = {};
    if (!ready(state)) {
        return false;
    }
    common.patchEpoch = state.binding.patchEpoch;
    common.entries[0].activitySessionId = state.binding.activitySessionId;
    common.entries[0].reconciliationGeneration = state.targetGeneration;
    common.entryCount = 1;
    return true;
}

/** Prepares one confirmed common-root packet contribution. */
bool prepare_common(const State& state, CommonPlan& plan) noexcept {
    plan = {};
    if (!ready(state) || state.commonCovered || state.commonPending
        || !confirmed_common(state, plan.common)) {
        return false;
    }
    plan.stateGeneration = state.generation;
    plan.reconciliationGeneration = state.targetGeneration;
    plan.prepared = true;
    return true;
}

/** Attaches one common root to one future eligible packet outcome. */
bool commit_common(State& state,
                   const CommonPlan& plan,
                   std::uint64_t packetGeneration,
                   CommonContributionHandle& handle) noexcept {
    handle = {};
    if (!plan.prepared || packetGeneration == 0 || !ready(state) || state.commonCovered
        || state.commonPending || plan.stateGeneration != state.generation
        || plan.reconciliationGeneration != state.targetGeneration
        || state.nextContributionGeneration == 0
        || state.nextContributionGeneration == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    middleware::gameplay::external::CommonState expected{};
    if (!confirmed_common(state, expected) || expected.patchEpoch != plan.common.patchEpoch
        || expected.entryCount != plan.common.entryCount
        || expected.entries[0].activitySessionId != plan.common.entries[0].activitySessionId
        || expected.entries[0].reconciliationGeneration
               != plan.common.entries[0].reconciliationGeneration) {
        return false;
    }
    const std::uint64_t generation = state.nextContributionGeneration++;
    state.pendingContributionGeneration = generation;
    state.pendingPacketGeneration = packetGeneration;
    state.commonPending = true;
    handle = {state.generation, generation, packetGeneration};
    return true;
}

/** Applies one exact common-root packet outcome once. */
bool settle_common(State& state,
                   CommonContributionHandle handle,
                   std::uint64_t packetGeneration,
                   CommonOutcome outcome) noexcept {
    if (!state.commonPending || handle.stateGeneration != state.generation || handle.generation == 0
        || handle.generation != state.pendingContributionGeneration || packetGeneration == 0
        || handle.packetGeneration != packetGeneration
        || state.pendingPacketGeneration != packetGeneration
        || (outcome != CommonOutcome::success && outcome != CommonOutcome::lost)) {
        return false;
    }
    state.commonPending = false;
    state.pendingContributionGeneration = 0;
    state.pendingPacketGeneration = 0;
    state.commonCovered = outcome == CommonOutcome::success;
    return true;
}

/** Reports whether a successful packet covered this exact common root. */
bool common_covered(const State& state) noexcept {
    return ready(state) && state.commonCovered && !state.commonPending;
}

/** Reports whether a later common root confirmed the queued generation. */
bool ready(const State& state) noexcept {
    return state.phase == Phase::confirmed && valid_binding(state.binding)
           && state.observedGeneration == state.targetGeneration;
}

} // namespace sunrise::server::gameplay::physics::common
