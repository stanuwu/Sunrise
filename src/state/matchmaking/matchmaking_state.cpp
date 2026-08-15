#include "matchmaking_state.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "../runtime/storage/internal.h"
#include "transactions/internal.h"

namespace sunrise::state::matchmaking {

/** Acquires the first available generation-checked logical context. */
bool acquire_context(ContextHandle& context) noexcept {
    context = {};
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    MatchmakingState& state = runtime::storage::g_state.matchmaking;
    if (state.allocatorRevision == kInvalidRevision) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    for (std::size_t index = 0; index < state.contexts.size(); ++index) {
        ContextSlot& slot = state.contexts[index];
        if (slot.active || slot.generation == (std::numeric_limits<std::uint32_t>::max)()) {
            continue;
        }
        // Advance before erasure so a released handle cannot become valid again.
        std::uint32_t generation = slot.generation;
        ++generation;
        SecureZeroMemory(&slot, sizeof slot);
        slot.active = true;
        slot.generation = generation;
        slot.data.revision = kInitialContextRevision;
        slot.data.latestSlot = kInvalidVariantSlot;
        context = {index, generation};
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return true;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return false;
}

/** Releases a context and securely erases every descriptor it owns. */
bool release_context(ContextHandle context) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ContextSlot* slot = transactions::resolve(runtime::storage::g_state.matchmaking, context);
    if (slot == nullptr) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Keep only the generation needed to reject handles from this acquisition.
    const std::uint32_t generation = slot->generation;
    SecureZeroMemory(slot, sizeof *slot);
    slot->generation = generation;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

/** Copies the latest advertisement under the State lock. */
bool latest_snapshot(ContextHandle context, LatestSnapshot& snapshot) noexcept {
    SecureZeroMemory(&snapshot, sizeof snapshot);
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    MatchmakingState& state = runtime::storage::g_state.matchmaking;
    ContextSlot* slot = transactions::resolve(state, context);
    if (slot == nullptr) {
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return false;
    }
    LatestSnapshot prepared{};
    // A kept variant wins once it replaces the standalone latest id.
    if (slot->data.latestSlot < kVariantCapacity) {
        const VariantRecord& latest = slot->data.variants[slot->data.latestSlot];
        if (!latest.occupied || latest.advertisementId == kAbsentAdvertisementId) {
            ReleaseSRWLockShared(&runtime::storage::g_stateLock);
            return false;
        }
        prepared.advertisementId = latest.advertisementId;
        prepared.hasDescriptor = latest.hasDescriptor;
        if (latest.hasDescriptor) {
            std::memcpy(prepared.descriptor.data(), latest.descriptor.data(), kDescriptorSize);
        }
    } else if (slot->data.latestSlot == kInvalidVariantSlot
               && slot->data.standaloneLatestId != kAbsentAdvertisementId) {
        prepared.advertisementId = slot->data.standaloneLatestId;
    } else {
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return false;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    snapshot = prepared;
    // Keep the descriptor in State and the caller output, not an extra stack lifetime.
    SecureZeroMemory(&prepared, sizeof prepared);
    return true;
}

/** Securely erases a transient latest-advertisement snapshot. */
void erase_snapshot(LatestSnapshot& snapshot) noexcept {
    SecureZeroMemory(&snapshot, sizeof snapshot);
}

} // namespace sunrise::state::matchmaking
