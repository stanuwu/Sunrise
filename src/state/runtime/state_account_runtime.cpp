#include <Windows.h>

#include <cstddef>
#include <cstdint>

#include "runtime.h"
#include "state.h"
#include "storage/internal.h"

namespace sunrise::state {

/** Stores the active account key without publishing an incomplete account. */
bool set_primary_soid(std::uint64_t primarySoid) noexcept {
    if (primarySoid == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    candidate.primarySoid = primarySoid;
    // Characters belong to the account key the Client uses. The reference account and its
    // characters differ only in the low byte, so the authored rows are rebased onto that key.
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        candidate.characters[index].soid = primarySoid + 1U + index;
    }
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Publish only after the settings and identity rules hold together.
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

/** Moves the selection to one authored character. */
bool set_selected_character(std::uint64_t characterSoid, bool& changed) noexcept {
    changed = false;
    if (characterSoid == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    std::size_t picked = candidate.characterCount;
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        if (candidate.characters[index].soid == characterSoid) {
            picked = index;
        }
    }
    if (picked == candidate.characterCount) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    const bool alreadySelected = candidate.characters[picked].selected;
    for (CharacterState& character : candidate.characters) {
        character.selected = false;
    }
    candidate.characters[picked].selected = true;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Publish only after the whole account still meets its identity rules.
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    changed = !alreadySelected;
    return true;
}

/** @return A copy of the active account state, read under the lock. */
AccountState account_snapshot() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const AccountState snapshot = runtime::storage::g_state.account;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return snapshot;
}

} // namespace sunrise::state
