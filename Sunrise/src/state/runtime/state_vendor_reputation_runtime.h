#pragma once

#include "../account/account_state.h"
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
    bool operator==(const VendorReputationAward&) const = default;
};

/** Captured payment and character state; no inventory item is granted by this transaction. */
struct PendingVendorReputation {
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeItems{};
    unlocks::ProgressionLanes beforeProgression{};
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

} // namespace sunrise::state
