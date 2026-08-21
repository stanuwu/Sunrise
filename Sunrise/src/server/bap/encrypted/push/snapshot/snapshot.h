#pragma once

#include <array>
#include <cstddef>

#include "../../../../../middleware/datagen/family4/loadout/definition.h"
#include "../../../../../middleware/queuez/queuez_update.h"
#include "../../../../../middleware/queuez/subscription.h"
#include "../../../../../state/account/account_state.h"
#include "../../internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

/** Account and selected-character identity take the first two family-four descriptors. */
inline constexpr std::size_t kFamily4IdentityObjectCount = 2;
/**
 * Family four carries both identity objects plus one record per equipped or unequipped character
 * item and every resident-backed profile stack. The character inventory, equip-summary, and
 * profile action-source readers all follow instance SOIDs, so every nonzero row key needs a
 * published record. The fixed profile-row capacity also bounds future runtime acquisitions.
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

/** Builds one initial family snapshot from State and build mappings. */
[[nodiscard]] bool prepare_initial(Scratch& scratch,
                                   const middleware::queuez::Subscription& subscription,
                                   Prepared& prepared) noexcept;

/** Rebuilds the active account family as a full snapshot at the peer's next version. */
[[nodiscard]] bool prepare_family4_refresh(Scratch& scratch,
                                           std::uint64_t familyRootSoid,
                                           std::int32_t version,
                                           Prepared& prepared) noexcept;

/** Builds a full next-version Family-4 snapshot from an uncommitted account after-image. */
[[nodiscard]] bool prepare_family4_refresh_from_account(Scratch& scratch,
                                                        std::uint64_t familyRootSoid,
                                                        std::int32_t version,
                                                        const state::AccountState& account,
                                                        Prepared& prepared) noexcept;

/** Builds the family-zero banner anchor and the record for the character it names. */
[[nodiscard]] bool prepare_banner(Scratch& scratch,
                                  std::uint64_t familyRootSoid,
                                  std::int32_t version,
                                  std::uint64_t previousCharacter,
                                  Prepared& prepared) noexcept;

/** Builds the one-record Family-0 incremental that refreshes rendered equipment in place. */
[[nodiscard]] bool
prepare_character_appearance_refresh(Scratch& scratch,
                                     const queuez::CharacterAppearanceRefresh& refresh,
                                     const state::CharacterState& afterCharacter,
                                     std::size_t characterIndex,
                                     std::uint8_t nativeEquipmentSlot,
                                     bool replaceCharacterRecord,
                                     Prepared& prepared) noexcept;

/** Builds one Family-3 appearance increment from an uncommitted character after-image. */
[[nodiscard]] bool prepare_roster_appearance_refresh(Scratch& scratch,
                                                     const queuez::RosterAppearanceRefresh& refresh,
                                                     const state::CharacterState& afterCharacter,
                                                     std::size_t characterIndex,
                                                     Prepared& prepared) noexcept;

} // namespace sunrise::server::bap::encrypted::push::snapshot
