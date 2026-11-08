#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/datagen/definitions.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

/** Log line capacity. The line carries one family number and nothing else. */
constexpr std::size_t kEmptyReportCapacity = 64;

/**
 * Builds one full family snapshot at the initial version from State and build mappings.
 * @param scratch Object and compression storage owned by the lock.
 * @param subscription Family and root to build.
 * @param acquisitionPresentationRows Row identities an open acquisition feed still shows.
 * @param accountSelectionCleared Family 4 only: the account object names no character.
 * @param prepared Gets the descriptors and scratch clear extents.
 * @return False when the snapshot, or its empty fallback, does not fit.
 */
[[nodiscard]] bool
build_full(Scratch& scratch,
           const middleware::queuez::Subscription& subscription,
           std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
           bool accountSelectionCleared,
           Prepared& prepared) noexcept {
    // Family zero never reaches here. It carries the banner pair, and its version and flags come
    // from the peer's own state, so the subscription path builds it directly.
    const Reservation reservation = reserve_prior(scratch, prepared);
    Prepared staged{};
    staged.rawClearSize = reservation.rawClearSize;
    staged.compressedClearSize = reservation.compressedClearSize;
    // Family two's directory and the family-three roster object both live in slot zero, so one
    // expression covers every family that reaches here.
    const std::uint32_t slotIndex = subscription.familyType == kAccountFamilyType
                                        ? kAccountDefinitionSlotIndex
                                        : kRosterDefinitionSlotIndex;
    std::uint32_t objectId = 0;
    const bool hasDefinition =
        middleware::datagen::object_id(subscription.familyType, slotIndex, objectId);
    bool success = false;
    if (subscription.familyType == queuez::kSocialRosterFamilyType && hasDefinition) {
        success = prepare_social_roster(scratch, subscription, objectId, reservation, staged);
    } else if (subscription.familyType == kRosterFamilyType && hasDefinition) {
        success = prepare_roster(scratch, subscription, objectId, reservation, staged);
    } else if (subscription.familyType == kAccountFamilyType && hasDefinition) {
        success = prepare(scratch,
                          subscription,
                          objectId,
                          reservation,
                          acquisitionPresentationRows,
                          accountSelectionCleared,
                          staged);
    }
    // A family with no generated objects falls back to an empty full snapshot.
    if (!success) {
        // Every builder failure lands here. An empty family four never sets `family4Active`, and
        // that refuses every later character pick.
        std::array<char, kEmptyReportCapacity> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=queuez stage=snapshot result=empty family=%u",
                                        static_cast<unsigned>(subscription.familyType));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
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
    }
    if (!commit(staged, prepared)) {
        // Clear the rejected staging tails but keep any prior published payload prefix.
        clear_after(scratch, reservation);
        return false;
    }
    return true;
}

} // namespace

/** Builds one full family snapshot at the initial version from State and build mappings. */
bool prepare_initial(
    Scratch& scratch,
    const middleware::queuez::Subscription& subscription,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept {
    return build_full(scratch, subscription, acquisitionPresentationRows, false, prepared);
}

/** Rebuilds the account family at an explicitly staged nonzero version. */
bool prepare_family4_refresh(
    Scratch& scratch,
    std::uint64_t familyRootSoid,
    std::int32_t version,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    bool accountSelectionCleared,
    Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion) {
        return false;
    }
    middleware::queuez::Subscription subscription{};
    subscription.familyType = kAccountFamilyType;
    subscription.familyRootSoid = familyRootSoid;
    if (!build_full(
            scratch, subscription, acquisitionPresentationRows, accountSelectionCleared, prepared)
        || prepared.family.objects.empty() || prepared.family.type != kAccountFamilyType
        || prepared.family.rootSoid != familyRootSoid
        || prepared.family.flags != middleware::queuez::kFullSnapshotFlag) {
        return false;
    }
    prepared.family.version = version;
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
