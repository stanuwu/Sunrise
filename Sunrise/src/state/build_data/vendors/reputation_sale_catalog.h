#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::vendors {

/** Bounds the build-era manifest's 95 candidate turn-ins; only verified rows are loaded. */
inline constexpr std::size_t kReputationSaleCapacity = 128;

/** One authored turn-in sale; the installed vendor row must match before it awards XP. */
struct ReputationSale {
    std::uint32_t vendorHash{};
    std::uint16_t saleIndex{};
    std::uint32_t factionHash{};
    std::uint32_t placeholderHash{};
    std::uint32_t costHash{};
    std::int32_t categoryIndex{};
    std::uint32_t costQuantity{};
    std::int32_t experiencePerUnit{};
};

/** Loads the build-scoped authored sales embedded in the DLL; no player save is opened. */
[[nodiscard]] bool load_reputation_sales(void* module) noexcept;

/** Withdraws every authored sale before build-data shutdown or failed publication. */
void clear_reputation_sales() noexcept;

/** Finds the authored sale for a vendor selector pair. */
[[nodiscard]] bool find_reputation_sale(std::uint32_t vendorHash,
                                        std::uint16_t saleIndex,
                                        ReputationSale& sale) noexcept;

/** Prevents a known reputation placeholder falling through to ordinary item acquisition. */
[[nodiscard]] bool is_reputation_placeholder(std::uint32_t vendorHash,
                                             std::uint32_t placeholderHash) noexcept;

} // namespace sunrise::state::build_data::vendors
