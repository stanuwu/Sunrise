#include "replay_high_water.h"

namespace sunrise::middleware::gameplay::dtls {

/** Applies one expanded sequence to the authenticated receive high-water mark. */
ReplayDecision update(ReplayHighWater& state, std::uint32_t sequence) noexcept {
    if (!state.initialized) {
        state.lastSequence = sequence;
        state.initialized = true;
        return ReplayDecision::accepted;
    }
    if (sequence == state.lastSequence) {
        return ReplayDecision::duplicate;
    }
    if (sequence < state.lastSequence) {
        return ReplayDecision::tooOld;
    }
    state.lastSequence = sequence;
    return ReplayDecision::accepted;
}

} // namespace sunrise::middleware::gameplay::dtls
