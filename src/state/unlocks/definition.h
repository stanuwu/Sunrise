#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::unlocks {

/** The account acquired-flag bank holds one byte per flag. */
inline constexpr std::size_t kAccountFlagCapacity = 12'300;
/** The profile unlock bank holds one byte per flag. */
inline constexpr std::size_t kProfileFlagCapacity = 512;
/** Each per-character unlock block holds one byte per flag. */
inline constexpr std::size_t kCharacterFlagCapacity = 256;
/** The account objective bank holds one signed value per objective. */
inline constexpr std::size_t kObjectiveValueCapacity = 6'200;
/** The selected-character object carries its own acquired-flag bank. */
inline constexpr std::size_t kCharacterObjectFlagCapacity = 4'096;
/** The selected-character object carries its own objective bank. */
inline constexpr std::size_t kCharacterObjectValueCapacity = 768;

/** A set acquired flag is stored as its biased 2-bit true value. */
inline constexpr std::uint8_t kFlagSet = 2;
/** A clear acquired flag is stored as zero. */
inline constexpr std::uint8_t kFlagClear = 0;

/**
 * Authored unlock policy published once for the process.
 * Every bank is expanded at parse time, so readers copy bytes with no run decoding.
 */
struct Table {
    std::array<std::uint8_t, kAccountFlagCapacity> accountFlags{};
    std::array<std::uint8_t, kProfileFlagCapacity> profileFlags{};
    std::array<std::uint8_t, kCharacterFlagCapacity> characterFlags{};
    std::array<std::int32_t, kObjectiveValueCapacity> objectiveValues{};
    std::array<std::uint8_t, kCharacterObjectFlagCapacity> characterObjectFlags{};
    std::array<std::int32_t, kCharacterObjectValueCapacity> characterObjectValues{};
};

} // namespace sunrise::state::unlocks
