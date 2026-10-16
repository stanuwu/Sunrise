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

/** Build-86657 Commander Zavala vendor definition. */
inline constexpr std::uint32_t kVanguardRankVendorHash = 69482069U;
/** Zavala's normal claim interaction, excluding the adjacent level warning. */
inline constexpr std::uint16_t kVanguardRankInteraction = 40;
/** Vendor-local category containing Zavala's rank packages. */
inline constexpr std::int32_t kVanguardRankCategory = 3;
/** Reconstructed rowless selection of Zavala's current package sale. */
inline constexpr std::uint16_t kVanguardRankSale = 93;

/** Build-86657 Lord Shaxx vendor definition. */
inline constexpr std::uint32_t kCrucibleRankVendorHash = 3603221665U;
/** Shaxx's normal claim interaction, excluding the adjacent level warning. */
inline constexpr std::uint16_t kCrucibleRankInteraction = 28;
/** Vendor-local category containing Shaxx's rank packages. */
inline constexpr std::int32_t kCrucibleRankCategory = 10;
/** Reconstructed rowless selection of Shaxx's current package sale. */
inline constexpr std::uint16_t kCrucibleRankSale = 96;

/** Build-86657 Banshee-44 vendor definition. */
inline constexpr std::uint32_t kGunsmithRankVendorHash = 672118013U;
/** Banshee's normal claim interaction, excluding the adjacent level warning. */
inline constexpr std::uint16_t kGunsmithRankInteraction = 35;
/** Vendor-local category containing Banshee's rank packages. */
inline constexpr std::int32_t kGunsmithRankCategory = 8;
/** Reconstructed rowless selection of Banshee's current package sale. */
inline constexpr std::uint16_t kGunsmithRankSale = 16;

inline constexpr std::array kReconstructedRankClaimLinks{
    ReconstructedRankClaimLink{kVanguardRankVendorHash,
                               kVanguardRankInteraction,
                               kVanguardRankCategory,
                               kVanguardRankSale},
    ReconstructedRankClaimLink{kCrucibleRankVendorHash,
                               kCrucibleRankInteraction,
                               kCrucibleRankCategory,
                               kCrucibleRankSale},
    ReconstructedRankClaimLink{kGunsmithRankVendorHash,
                               kGunsmithRankInteraction,
                               kGunsmithRankCategory,
                               kGunsmithRankSale},
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
