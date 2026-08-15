#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::progressions {

/** The shipped build declares 88 progressions. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 256;

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

/** One progression definition, reduced to what routes its record. */
struct Definition {
    std::uint16_t definitionIndex{};
    Scope scope{Scope::unreplicated};
};

} // namespace sunrise::state::build_data::progressions
