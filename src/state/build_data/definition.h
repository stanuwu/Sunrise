#pragma once

#include <cstdint>

namespace sunrise::state::build_data {

/** PE fields and a configured-equipment hash tie the generated mappings to one build. */
struct BuildIdentity {
    std::uint32_t imageTimestamp{};
    std::uint32_t imageSize{};
    /** Authored item levels and the light-stat rules, so a mismatched cache is not reused. */
    std::uint64_t configuredEquipmentHash{};

    /** @return True when every field matches. */
    [[nodiscard]] constexpr bool operator==(const BuildIdentity& other) const noexcept = default;
};

} // namespace sunrise::state::build_data
