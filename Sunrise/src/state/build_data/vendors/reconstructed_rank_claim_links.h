#pragma once

#include <array>
#include <cstdint>

namespace sunrise::state::build_data::vendors {

/** Build-86657 rank credit links also supply sales omitted by rowless replies. */
struct ReconstructedRankClaimLink {
    std::uint32_t vendorHash;
    std::uint16_t interactionIndex;
    std::int32_t categoryIndex;
    std::uint16_t saleIndex;
};

/** Zavala's rank claim links interaction 40 in category 3 to package sale 93. */
inline constexpr ReconstructedRankClaimLink kVanguardRankClaimLink{69482069U, 40, 3, 93};
/** Shaxx's rank claim links interaction 28 in category 10 to package sale 96. */
inline constexpr ReconstructedRankClaimLink kCrucibleRankClaimLink{3603221665U, 28, 10, 96};
/** Banshee's rank claim links interaction 35 in category 8 to package sale 16. */
inline constexpr ReconstructedRankClaimLink kGunsmithRankClaimLink{672118013U, 35, 8, 16};

inline constexpr std::array kReconstructedRankClaimLinks{
    kVanguardRankClaimLink,
    kCrucibleRankClaimLink,
    kGunsmithRankClaimLink,
};

/**
 * Finds the reconstructed claim link for one installed vendor.
 * @param vendorHash Installed vendor definition hash.
 * @return The link, or null for vendors without a supported rank claim.
 */
[[nodiscard]] constexpr const ReconstructedRankClaimLink*
find_reconstructed_rank_claim_link(std::uint32_t vendorHash) noexcept {
    for (const ReconstructedRankClaimLink& link : kReconstructedRankClaimLinks) {
        if (link.vendorHash == vendorHash) {
            return &link;
        }
    }
    return nullptr;
}

} // namespace sunrise::state::build_data::vendors
