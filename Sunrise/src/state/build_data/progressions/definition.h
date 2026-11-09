#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::progressions {

/** The shipped build declares 88 progressions. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 256;

/** Rank steps across every progression. The shipped build declares 1097. */
inline constexpr std::size_t kStepCapacity = 2048;

/** Steps one progression may declare. The longest shipped ladder has 100 ranks. */
inline constexpr std::size_t kStepPerDefinitionCapacity = 255;

/**
 * Which replicated object holds a progression's record array.
 * The scope decides the array, and the definition's position among the definitions sharing that
 * scope decides the slot inside it.
 */
enum class Scope : std::uint8_t {
    /** The account object's progression bank. */
    account = 0,
    /** The character object's progression bank. */
    character = 1,
    /** Any other scope belongs to an object this server does not replicate. */
    unreplicated = 2,
};

/** One rank step of one progression: what the rank after it costs. */
struct Step {
    /** Experience this rank costs. Rank 1 costs nothing, so the first step is zero. */
    std::int32_t cost{};
};

/** One progression definition, reduced to what routes its record and what its ranks cost. */
struct Definition {
    std::uint16_t definitionIndex{};
    /** First row of this definition's range in the flat step bank. */
    std::uint16_t stepOffset{};
    std::uint8_t stepCount{};
    Scope scope{Scope::unreplicated};
    std::uint32_t definitionHash{};
};

} // namespace sunrise::state::build_data::progressions
