#include "queuez_state_validation.h"

#include <cstddef>

namespace sunrise::server::bap::encrypted::queuez {
namespace {

/** @return True for every implemented post-change roster phase. */
[[nodiscard]] bool valid_phase(Family3Phase phase) noexcept {
    return phase == Family3Phase::normal || phase == Family3Phase::publishOnce
           || phase == Family3Phase::responseOnly;
}

} // namespace

/** @return True when every active field and resident id make a canonical peer state. */
bool valid(const SessionState& state) noexcept {
    if (!valid_phase(state.family3Phase)) {
        return false;
    }
    // Family zero holds no resident manifest, so its whole contract is the version ladder. An
    // inactive family carries nothing. An active one names a character and never goes back.
    if (state.family0Active) {
        if (state.family0Character == 0 || state.family0Version < kInitialFamilyVersion) {
            return false;
        }
    } else if (state.family0Character != 0 || state.family0Version != kInitialFamilyVersion) {
        return false;
    }
    if (!state.family4Active) {
        if (state.family4RootSoid != 0 || state.family4Version != kInitialFamilyVersion
            || state.family4ResidentCount != 0 || state.family3Phase != Family3Phase::normal) {
            return false;
        }
        for (const ResidentObject& object : state.family4Residents) {
            if (object.objectSoid != 0 || object.definitionId != 0) {
                return false;
            }
        }
        return true;
    }
    if (state.family4RootSoid == 0 || state.family4ResidentCount == 0
        || state.family4ResidentCount > state.family4Residents.size()) {
        return false;
    }
    // Version zero is the full snapshot, and the roster phase leaves normal only once an
    // increment has gone out. Above zero the ladder is open. Opcode 505 and every ws-504
    // character move add one, so there is no fixed top.
    if (state.family4Version < kInitialFamilyVersion
        || (state.family4Version == kInitialFamilyVersion
            && state.family3Phase != Family3Phase::normal)) {
        return false;
    }
    for (std::size_t index = 0; index < state.family4ResidentCount; ++index) {
        const ResidentObject& object = state.family4Residents[index];
        if (object.objectSoid == 0 || object.definitionId == 0) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (state.family4Residents[prior].objectSoid == object.objectSoid) {
                return false;
            }
        }
    }
    for (std::size_t index = state.family4ResidentCount; index < state.family4Residents.size();
         ++index) {
        const ResidentObject& object = state.family4Residents[index];
        if (object.objectSoid != 0 || object.definitionId != 0) {
            return false;
        }
    }
    return state.family4Residents.front().objectSoid == state.family4RootSoid;
}

} // namespace sunrise::server::bap::encrypted::queuez
