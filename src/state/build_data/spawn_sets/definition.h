#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::spawn_sets {

/**
 * Map-package stems the class sweep finds. The live count is 77.
 * A stem is the key a destination joins its spawn sets by.
 */
inline constexpr std::size_t kStemCapacity = 128;
/** The longest installed stem is `mercury_destination` at 19 bytes. */
inline constexpr std::size_t kStemNameCapacity = 32;
/**
 * Distinct spawn-name hashes across every stem. The live count is 2,090 and the widest single
 * stem declares 294.
 */
inline constexpr std::size_t kNameHashCapacity = 4'096;
/** A bubble mask is 32 bytes, one bit per map-global bubble index. */
inline constexpr std::size_t kBubbleMaskBytes = 32;
/**
 * Activity packages one name hash is declared in. The widest hash of one stem is declared in 11,
 * and a hash above this keeps its first rows and sets the overflow flag.
 */
inline constexpr std::size_t kPackageCapacity = 12;

/** One map-package stem and the contiguous name-hash range it owns. */
struct Stem {
    /** Lowercase stem bytes without the `w64_` prefix or the package id suffix. */
    std::array<char, kStemNameCapacity> name{};
    /** Spawn points across every set of this stem. */
    std::uint32_t pointCount{};
    /** Spawn-set tags this stem declares. */
    std::uint16_t setCount{};
    /** First row of this stem's range in the flat name-hash bank. */
    std::uint16_t nameHashOffset{};
    std::uint16_t nameHashCount{};
    std::uint8_t nameLength{};
};

/** One distinct spawn-set name hash inside one stem. */
struct NameHash {
    /** FNV-1 of the set name. The client filters its spawn search by this value. */
    std::uint32_t value{};
    /** Spawn points of this stem that carry the hash. */
    std::uint32_t pointCount{};
    std::uint16_t stemIndex{};
    /**
     * Bubbles that offer this set, as map-global indices.
     * A spawn tag is wrapped by a component and held by a container, and the container carries the
     * mask. This is the union over every tag of this stem that declares the hash.
     */
    std::array<std::uint8_t, kBubbleMaskBytes> bubbleMask{};
    /**
     * One when some tag declaring this hash reached no container, so it carries no mask.
     * Every installed set is bound, so this stays clear. An unbound set has nothing to narrow it,
     * so it is offered for every bubble of the map rather than hidden.
     */
    std::uint8_t unbound{};
    /** One when a map package declares this hash, so the destination loads it with the geometry. */
    std::uint8_t inMapPackage{};
    /** Activity packages declaring it, which a destination loads only when it names them. */
    std::uint8_t activityPackageCount{};
    /** One when more activity packages declare it than the row can hold. */
    std::uint8_t activityPackageOverflow{};
    std::array<std::uint16_t, kPackageCapacity> activityPackages{};
};

} // namespace sunrise::state::build_data::spawn_sets
