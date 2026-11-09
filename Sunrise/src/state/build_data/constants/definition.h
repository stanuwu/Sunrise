#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::constants {

/** Character stat rows the installed investment constants blob names. */
inline constexpr std::size_t kCharacterStatRowCount = 6;
/** Build-86657 native stat vectors contain 64 indexed values. */
inline constexpr std::size_t kStatRowCount = 64;
/** Each character's value block in the account object holds 256 values. */
inline constexpr std::size_t kCharacterValueRowCount = 256;
/** A value the value maps place in no character's block. */
inline constexpr std::uint16_t kUnboundValueRow = 0xFFFFU;

/**
 * Stat rows the client searches a character's stat table by.
 * Row 0 is a real stat, so there is no unset value. `extracted` says whether a row is here.
 */
struct InvestmentConstants {
    bool extracted{};
    /** Row the banner's power number is searched by. */
    std::uint8_t lightStatRow{};
    /** Row converted from displayed weapon Power into native damage Power. */
    std::uint8_t weaponPowerStatRow{};
    /** The 6 rows the character sheet shows, in the blob's own order. */
    std::array<std::uint8_t, kCharacterStatRowCount> characterStatRows{};
    /** Row of each character's value block that holds the Synthesizer tier, or unbound. */
    std::uint16_t synthesizerTierValueRow{kUnboundValueRow};
};

/**
 * Requires the extracted weapon row to address the native stat vector, and a bound value row to
 * address the character value block.
 */
[[nodiscard]] constexpr bool valid(const InvestmentConstants& value) noexcept {
    return value.extracted && value.weaponPowerStatRow < kStatRowCount
           && (value.synthesizerTierValueRow == kUnboundValueRow
               || value.synthesizerTierValueRow < kCharacterValueRowCount);
}

} // namespace sunrise::state::build_data::constants
