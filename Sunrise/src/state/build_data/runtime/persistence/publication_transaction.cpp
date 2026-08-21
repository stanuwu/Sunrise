#include "publication_transaction.h"

#include <Windows.h>

#include "build_data_persistence.h"

namespace sunrise::state::build_data::runtime::persistence {

/** Takes the persistence lock unless a saved cache already freezes all domains. */
Transaction::Transaction() noexcept : state_(&context()) {
    AcquireSRWLockExclusive(&state_->lock);
    if (state_->persisted) {
        // A saved cache is one fixed build snapshot, so later extraction must not change State.
        ReleaseSRWLockExclusive(&state_->lock);
        state_ = nullptr;
        return;
    }
    active_ = true;
}

/** Releases a held publish lock when the caller exits before finishing. */
Transaction::~Transaction() {
    if (active_) {
        ReleaseSRWLockExclusive(&state_->lock);
    }
}

/** @return True when a catalog replace is allowed under the held State lock. */
bool Transaction::active() const noexcept {
    return active_;
}

/** Finishes one replace, and drops its target when the first cache write fails. */
bool Transaction::finish(bool published, Rollback& rollback) noexcept {
    if (!active_) {
        return false;
    }
    bool result = published;
    if (result && !persist_if_ready_locked(*state_, true)) {
        // A failed write must not leave the candidate visible as a published domain.
        rollback();
        result = false;
    }
    ReleaseSRWLockExclusive(&state_->lock);
    active_ = false;
    state_ = nullptr;
    return result;
}

} // namespace sunrise::state::build_data::runtime::persistence
