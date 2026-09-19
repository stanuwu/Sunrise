#pragma once

#include <array>
#include <cstddef>

#include "../../../../../middleware/datagen/family4/loadout/definition.h"
#include "../../../../../middleware/queuez/queuez_update.h"
#include "../../../../../middleware/queuez/subscription.h"
#include "../../../../../state/account/account_state.h"
#include "../../internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

/** Account and selected-character identity occupy the first two Family-4 descriptors. */
inline constexpr std::size_t kFamily4IdentityObjectCount = 2;
/**
 * Identity objects plus every character instance and resident-backed profile stack.
 * Every nonzero inventory SOID needs a published object.
 */
inline constexpr std::size_t kObjectCapacity =
    kFamily4IdentityObjectCount
    + state::kCharacterCapacity * middleware::datagen::family4::loadout::kItemCapacity
    + state::account::inventory::kProfileActionSourceCapacity;

/** Prepared descriptors and scratch extents owned until the update codec copies their bodies. */
struct Prepared {
    std::array<middleware::queuez::Object, kObjectCapacity> objects{};
    middleware::queuez::Family family{};
    std::size_t rawClearSize{};
    std::size_t compressedClearSize{};

    // Default copying would leave family.objects pointing into the source descriptor array.
    Prepared() noexcept = default;
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    Prepared(Prepared&&) = delete;
    Prepared& operator=(Prepared&&) = delete;
};

/**
 * Builds one initial family snapshot from State and build mappings.
 * @param scratch Object and compression storage owned by the lock.
 * @param subscription Family id the Client picked.
 * @param prepared Gets the object descriptors and scratch clear extents.
 * @return True when the asked-for snapshot is valid for the current State and mappings.
 */
[[nodiscard]] bool
prepare_initial(Scratch& scratch,
                const middleware::queuez::Subscription& subscription,
                std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
                Prepared& prepared) noexcept;

/**
 * Folds the family-two member fields the projected public profile does not carry.
 * The account's seat and its established fireteam both change without the owner's profile
 * moving, so the poll that refreshes a roster row cannot see them through the profile
 * generation alone.
 * @param scratch Account scratch owned by the caller under the BAP session lock.
 * @param familyRootSoid Account root the subscription names.
 * @return A value that changes only when the served member record would change.
 */
[[nodiscard]] std::uint32_t social_roster_revision(Scratch& scratch,
                                                   std::uint64_t familyRootSoid) noexcept;

/** Rebuilds the active account family at the peer's next version. */
[[nodiscard]] bool prepare_family4_refresh(
    Scratch& scratch,
    std::uint64_t familyRootSoid,
    std::int32_t version,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept;

/**
 * Builds the family-zero banner anchor and the record for the character it names.
 * @param scratch Raw object storage owned by the lock.
 * @param familyRootSoid Root the Client subscribed for the roster.
 * @param version Family version this frame carries.
 * @param previousCharacter Character whose record this frame releases, or zero for the full
 *        snapshot. Nonzero also clears the full-snapshot flag, which retail sets once.
 * @param prepared Gets the descriptors and the scratch clear extent.
 * @return True when a character is selected and every object fits raw storage.
 */
[[nodiscard]] bool prepare_banner(Scratch& scratch,
                                  std::uint64_t familyRootSoid,
                                  std::int32_t version,
                                  std::uint64_t previousCharacter,
                                  Prepared& prepared) noexcept;

/**
 * Builds the one-record Family-0 incremental that refreshes rendered equipment in place.
 * Encoded from the prepared after-image: the transaction commits only once both frames fit.
 * @param scratch Raw object storage owned by the lock.
 * @param refresh Family-0 root, version, and resident the incremental is built against.
 * @param afterCharacter Prepared State after-image the record is encoded from.
 * @param characterIndex Position of that character in the account.
 * @param nativeEquipmentSlot Native slot whose rendered item changed.
 * @param replaceCharacterRecord Recreate the resident record so a same-instance shader or
 *        ornament change rebuilds the render binding.
 * @param prepared Gets the descriptors and the scratch clear extent.
 * @return True when the character resolves and every object fits raw storage.
 */
[[nodiscard]] bool
prepare_character_appearance_refresh(Scratch& scratch,
                                     const queuez::CharacterAppearanceRefresh& refresh,
                                     const state::CharacterState& afterCharacter,
                                     std::size_t characterIndex,
                                     std::uint8_t nativeEquipmentSlot,
                                     bool replaceCharacterRecord,
                                     Prepared& prepared) noexcept;

/**
 * Builds one Family-3 appearance increment from an uncommitted character after-image.
 * The
 * character record is always first; when requested, the changed account roster follows it.
 */
[[nodiscard]] bool prepare_roster_appearance_refresh(Scratch& scratch,
                                                     const queuez::RosterAppearanceRefresh& refresh,
                                                     const state::CharacterState& afterCharacter,
                                                     std::size_t characterIndex,
                                                     Prepared& prepared) noexcept;

} // namespace sunrise::server::bap::encrypted::push::snapshot
