#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::constants {

/** Character stat rows the installed investment constants blob names. */
inline constexpr std::size_t kCharacterStatRowCount = 6;

/**
 * Stat rows the client searches a character's stat table by.
 * Row 0 is a real stat, so there is no unset value. `extracted` says whether a row is here.
 */
struct InvestmentConstants {
    bool extracted{};
    /** Row the banner's power number is searched by. */
    std::uint8_t lightStatRow{};
    /** The 6 rows the character sheet shows, in the blob's own order. */
    std::array<std::uint8_t, kCharacterStatRowCount> characterStatRows{};
};

} // namespace sunrise::state::build_data::constants
