#include "../../../../../middleware/datagen/definitions.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

/** Builds one full family snapshot at the initial version from State and build mappings. */
bool prepare_initial(Scratch& scratch,
                     const middleware::queuez::Subscription& subscription,
                     Prepared& prepared) noexcept {
    // Family zero never reaches here. It carries the banner pair, and its version and flags come
    // from the peer's own state, so the subscription path builds it directly.
    const Reservation reservation = reserve_prior(scratch, prepared);
    Prepared staged{};
    staged.rawClearSize = reservation.rawClearSize;
    staged.compressedClearSize = reservation.compressedClearSize;
    const std::uint32_t slotIndex = subscription.familyType == kAccountFamilyType
                                        ? kAccountDefinitionSlotIndex
                                        : kRosterDefinitionSlotIndex;
    std::uint32_t objectId = 0;
    const bool hasDefinition =
        middleware::datagen::object_id(subscription.familyType, slotIndex, objectId);
    bool success = false;
    if (subscription.familyType == kRosterFamilyType && hasDefinition) {
        success = prepare_roster(scratch, subscription, objectId, reservation, staged);
    } else if (subscription.familyType == kAccountFamilyType && hasDefinition) {
        success = prepare(scratch, subscription, objectId, reservation, staged);
    }
    // A family with no generated objects still publishes an empty full snapshot. That promotes the
    // record without claiming a manifest.
    if (!success) {
        // A failed preparation may have staged descriptors, so start the fallback clean.
        staged.objects = {};
        staged.rawClearSize = reservation.rawClearSize;
        staged.compressedClearSize = reservation.compressedClearSize;
        staged.family = middleware::queuez::Family{
            subscription.familyType,
            subscription.familyRootSoid,
            kInitialFamilyVersion,
            middleware::queuez::kFullSnapshotFlag,
            {},
        };
        success = true;
    }
    if (!success || !commit(staged, prepared)) {
        // Clear the rejected staging tails but keep any prior published payload prefix.
        clear_after(scratch, reservation);
        return false;
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
