#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::hash_names {

/**
 * Hashes the table can hold: every distinct bubble name and every distinct spawn-set name.
 * The live counts measured are 431 bubbles and 1,897 spawn sets.
 */
inline constexpr std::size_t kNameCapacity = 4'096;
/** The longest resolved name measured is 30 characters. */
inline constexpr std::size_t kNameLength = 48;

/** One name hash and the internal name that produces it. */
struct Name {
    /** FNV-1 of the name below, which is the hash a bubble or a spawn set carries. */
    std::uint32_t hash{};
    /** Lowercase internal name, with no null. */
    std::array<char, kNameLength> name{};
    std::uint8_t nameLength{};
};

/**
 * Tests whether one byte may appear in an internal name.
 * The names are lowercase identifiers, so any other byte ends a candidate instead of joining it.
 * @param value Candidate byte.
 * @return True for a lowercase letter, a digit, or an underscore.
 */
[[nodiscard]] constexpr bool name_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_';
}

} // namespace sunrise::state::build_data::hash_names
