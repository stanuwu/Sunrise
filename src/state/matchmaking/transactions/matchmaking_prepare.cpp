#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../../runtime/storage/internal.h"
#include "../matchmaking_state.h"
#include "internal.h"

namespace sunrise::state::matchmaking {

/** Prepares a variant update without changing stored State. */
bool prepare_variant_update(ContextHandle context,
                            std::uint64_t requestId,
                            std::uint64_t variant,
                            bool hasDescriptor,
                            std::span<const std::byte> descriptor,
                            std::uint64_t& advertisementId,
                            PendingMutation& mutation) noexcept {
    advertisementId = kAbsentAdvertisementId;
    mutation = {};
    if ((hasDescriptor && (descriptor.data() == nullptr || descriptor.size() != kDescriptorSize))
        || (!hasDescriptor && !descriptor.empty())) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    MatchmakingState& state = runtime::storage::g_state.matchmaking;
    ContextSlot* slot = transactions::resolve(state, context);
    if (slot == nullptr || slot->data.revision == (std::numeric_limits<std::uint64_t>::max)()) {
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return false;
    }
    const std::size_t existing = transactions::find_variant(slot->data, variant);
    const bool targetsExisting = existing != transactions::no_variant();
    const std::size_t target =
        targetsExisting ? existing : transactions::select_variant_slot(slot->data);
    const bool consumesAllocator = requestId == kAbsentAdvertisementId && !targetsExisting;
    if (consumesAllocator && !transactions::allocator_available(state)) {
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return false;
    }
    // A request id wins, then an existing variant id, then the global allocator.
    const std::uint64_t selectedId =
        requestId != kAbsentAdvertisementId
            ? requestId
            : (targetsExisting ? slot->data.variants[existing].advertisementId
                               : state.nextAdvertisementId);
    if (selectedId == kAbsentAdvertisementId) {
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return false;
    }
    PendingMutation prepared{};
    prepared.kind = MutationKind::updateVariant;
    prepared.context = context;
    prepared.expectedContextRevision = slot->data.revision;
    prepared.expectedAllocatorRevision = state.allocatorRevision;
    prepared.expectedNextAdvertisementId = state.nextAdvertisementId;
    prepared.requestId = requestId;
    prepared.advertisementId = selectedId;
    prepared.variant = variant;
    prepared.targetSlot = static_cast<std::uint8_t>(target);
    prepared.consumesAllocator = consumesAllocator;
    prepared.targetsExistingVariant = targetsExisting;
    prepared.hasDescriptor = hasDescriptor;
    prepared.descriptorData = hasDescriptor ? descriptor.data() : nullptr;
    prepared.descriptorSize = hasDescriptor ? descriptor.size() : 0;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    advertisementId = selectedId;
    mutation = prepared;
    return true;
}

/** Finds the latest id, or prepares the first standalone id. */
bool prepare_initial_latest(ContextHandle context,
                            std::uint64_t& advertisementId,
                            PendingMutation& mutation) noexcept {
    advertisementId = kAbsentAdvertisementId;
    mutation = {};
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    MatchmakingState& state = runtime::storage::g_state.matchmaking;
    ContextSlot* slot = transactions::resolve(state, context);
    if (slot == nullptr) {
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return false;
    }
    // A kept variant wins, then a standalone id. Only an empty context allocates.
    if (slot->data.latestSlot < kVariantCapacity) {
        const VariantRecord& latest = slot->data.variants[slot->data.latestSlot];
        if (!latest.occupied || latest.advertisementId == kAbsentAdvertisementId) {
            ReleaseSRWLockShared(&runtime::storage::g_stateLock);
            return false;
        }
        advertisementId = latest.advertisementId;
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return true;
    }
    if (slot->data.latestSlot != kInvalidVariantSlot) {
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return false;
    }
    if (slot->data.standaloneLatestId != kAbsentAdvertisementId) {
        advertisementId = slot->data.standaloneLatestId;
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return true;
    }
    if (slot->data.revision == (std::numeric_limits<std::uint64_t>::max)()
        || !transactions::allocator_available(state)) {
        ReleaseSRWLockShared(&runtime::storage::g_stateLock);
        return false;
    }
    PendingMutation prepared{};
    prepared.kind = MutationKind::createStandaloneLatest;
    prepared.context = context;
    prepared.expectedContextRevision = slot->data.revision;
    prepared.expectedAllocatorRevision = state.allocatorRevision;
    prepared.expectedNextAdvertisementId = state.nextAdvertisementId;
    prepared.advertisementId = state.nextAdvertisementId;
    prepared.consumesAllocator = true;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    advertisementId = prepared.advertisementId;
    mutation = prepared;
    return true;
}

} // namespace sunrise::state::matchmaking
