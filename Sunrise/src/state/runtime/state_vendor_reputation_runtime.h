#pragma once

#include "../account/account_state.h"
#include "../build_data/progressions/definition.h"
#include "../unlocks/definition.h"

namespace sunrise::state {

/** A recognized reputation placeholder must never fall through to an item grant. */
enum class VendorReputationDisposition : std::uint8_t { notApplicable, refused, prepared };

/** Installed sale cost and its build-matched faction award. */
struct VendorReputationAward {
    std::uint32_t costHash{};
    std::uint32_t costQuantity{};
    std::int32_t experience{};
    std::uint16_t progressionIndex{};
    /** Character-object value row; used only when rankStepCount is positive. */
    std::uint16_t rewardValueRow{};
    /** Installed rank costs captured for commit revalidation. */
    std::array<std::int32_t, build_data::progressions::kStepPerDefinitionCapacity> rankStepCosts{};
    /** Zero preserves XP-only handling for factions without a supported reward rule. */
    std::size_t rankStepCount{};
    /** Build-86657 progression metadata marks these faction ladders as repeating. */
    bool repeatLastStep{};
    bool operator==(const VendorReputationAward&) const = default;
};

/** Captured payment and XP; rank rewards commit with the same turn-in. */
struct PendingVendorReputation {
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeItems{};
    unlocks::ProgressionLanes beforeProgression{};
    std::int32_t beforeRewardCredits{};
    VendorReputationAward award{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::size_t characterIndex{};
    std::size_t beforeItemCount{};
    std::uint16_t vendorIndex{};
    std::uint16_t saleIndex{};
    bool prepared{};
};

[[nodiscard]] VendorReputationDisposition prepare_vendor_reputation(
    std::uint16_t vendorIndex, std::uint16_t saleIndex, PendingVendorReputation& mutation) noexcept;
[[nodiscard]] bool commit_vendor_reputation(PendingVendorReputation& mutation) noexcept;

/** Build-86657 VALUE[927] reads character-object value row 55 for Vanguard claims. */
inline constexpr std::uint16_t kVanguardRewardValueRow = 55;

/** Positive credits identify a prepared claim; zero leaves ordinary rewards unchanged. */
struct VendorRewardClaim {
    std::int32_t beforeCredits{};
    std::uint16_t vendorIndex{};
    std::uint16_t saleIndex{};
    /** Rechecked against the vendor binding before preview or commit. */
    std::uint16_t rewardValueRow{};
};

struct PendingRecordRewardGrant;
[[nodiscard]] VendorReputationDisposition
prepare_vendor_reward_sale(std::uint16_t vendorIndex,
                           std::uint16_t saleIndex,
                           PendingRecordRewardGrant& mutation) noexcept;
[[nodiscard]] VendorReputationDisposition
prepare_vendor_reward(std::uint16_t vendorIndex,
                      std::uint16_t interactionIndex,
                      std::uint16_t replyIndex,
                      PendingRecordRewardGrant& mutation) noexcept;
[[nodiscard]] bool vendor_reward_current(const VendorRewardClaim& claim) noexcept;
[[nodiscard]] bool is_vendor_reward_category(std::uint16_t vendorIndex,
                                             std::int32_t categoryIndex) noexcept;

} // namespace sunrise::state
