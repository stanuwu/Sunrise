#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "../../runtime/storage/internal.h"
#include "../matchmaking_state.h"
#include "internal.h"

namespace sunrise::state::matchmaking {

/** Commits one prepared mutation only when all captured revisions still match. */
bool commit(PendingMutation& mutation) noexcept {
    // Take the view before the checks, so a failed transaction cannot be retried stale.
    const PendingMutation prepared = mutation;
    mutation = {};
    if (prepared.kind == MutationKind::none) {
        return true;
    }
    if (prepared.advertisementId == kAbsentAdvertisementId
        || !transactions::valid_descriptor(prepared)) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    MatchmakingState& state = runtime::storage::g_state.matchmaking;
    ContextSlot* slot = transactions::resolve(state, prepared.context);
    if (slot == nullptr || slot->data.revision != prepared.expectedContextRevision
        || slot->data.revision == (std::numeric_limits<std::uint64_t>::max)()) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    if (prepared.consumesAllocator
        && (!transactions::allocator_available(state)
            || state.allocatorRevision != prepared.expectedAllocatorRevision
            || state.nextAdvertisementId != prepared.expectedNextAdvertisementId
            || prepared.advertisementId != state.nextAdvertisementId)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    if (prepared.kind == MutationKind::createStandaloneLatest) {
        // Only current State can refuse: a variant or standalone id that arrived after the plan
        // was captured already owns the latest slot.
        if (slot->data.latestSlot != kInvalidVariantSlot
            || slot->data.standaloneLatestId != kAbsentAdvertisementId) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        ++slot->data.revision;
        slot->data.standaloneLatestId = prepared.advertisementId;
        transactions::advance_allocator(state);
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return true;
    }
    if (prepared.kind != MutationKind::updateVariant || prepared.targetSlot >= kVariantCapacity) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Redo the choice under the lock, so prepared data cannot redirect stored State.
    const std::size_t existing = transactions::find_variant(slot->data, prepared.variant);
    const bool targetsExisting = existing != transactions::no_variant();
    const std::size_t expectedTarget =
        targetsExisting ? existing : transactions::select_variant_slot(slot->data);
    const bool shouldConsume = prepared.requestId == kAbsentAdvertisementId && !targetsExisting;
    const std::uint64_t expectedId =
        prepared.requestId != kAbsentAdvertisementId
            ? prepared.requestId
            : (targetsExisting ? slot->data.variants[existing].advertisementId
                               : state.nextAdvertisementId);
    if (prepared.targetSlot != expectedTarget || prepared.targetsExistingVariant != targetsExisting
        || prepared.consumesAllocator != shouldConsume || prepared.advertisementId != expectedId) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    VariantRecord& record = slot->data.variants[expectedTarget];
    if (!targetsExisting) {
        SecureZeroMemory(&record, sizeof record);
    }
    if (prepared.hasDescriptor) {
        SecureZeroMemory(record.descriptor.data(), record.descriptor.size());
        std::memcpy(record.descriptor.data(), prepared.descriptorData, kDescriptorSize);
        record.hasDescriptor = true;
    }
    record.occupied = true;
    record.advertisementId = prepared.advertisementId;
    record.variant = prepared.variant;
    ++slot->data.revision;
    record.lastUpdatedRevision = slot->data.revision;
    slot->data.latestSlot = prepared.targetSlot;
    slot->data.standaloneLatestId = kAbsentAdvertisementId;
    if (prepared.consumesAllocator) {
        transactions::advance_allocator(state);
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

} // namespace sunrise::state::matchmaking
