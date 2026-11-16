#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::abilities {

/** A character record publishes 12 ability buckets with fixed meanings. */
inline constexpr std::size_t kBucketCapacity = 12;
/** Each bucket holds 16 definition hashes. */
inline constexpr std::size_t kBucketHashCapacity = 16;
/** The flat overflow bank holds 32 hashes no bucket category claims. */
inline constexpr std::size_t kOverflowCapacity = 32;
/**
 * A subclass publishes up to 16 sandbox perks, a third of the character perk bank, so the equipped
 * items always keep the rest. No shipped subclass declares more than 13 under one selection.
 */
inline constexpr std::size_t kPerkCapacity = 16;
/**
 * One row per subclass and selection in use: 3 characters by 3 owned subclasses by 2 rows, the
 * default selection and the remembered picks, which are kept per subclass item.
 */
inline constexpr std::size_t kDefinitionCapacity = 18;
/** All bits set marks a bucket no entry claimed. */
inline constexpr std::uint8_t kEmptyBucketKind = 0xFF;

/** One ability bucket: the item category it collects and that category's definition hashes. */
struct Bucket {
    std::uint8_t kind{kEmptyBucketKind};
    std::uint8_t hashCount{};
    std::array<std::uint32_t, kBucketHashCapacity> hashes{};
};

/**
 * The 5 socket entries one character has selected on its subclass.
 * Sprint is the sixth selected entry but is not selectable, so it is fixed and not a key field.
 */
struct Selection {
    std::uint8_t movementEntry{};
    std::uint8_t grenadeEntry{};
    std::uint8_t superEntry{};
    std::uint8_t meleeEntry{};
    std::uint8_t classEntry{};

    friend bool operator==(const Selection&, const Selection&) = default;
};

/**
 * The ability buckets one subclass publishes under one ability selection.
 * Both key fields are needed. The socket entry list is the subclass's ability layout, and the
 * selection picks among the choices that layout offers.
 */
struct Definition {
    std::uint16_t socketEntryListIndex{};
    Selection selection{};
    std::uint8_t overflowCount{};
    std::uint8_t perkCount{};
    std::array<Bucket, kBucketCapacity> buckets{};
    std::array<std::uint32_t, kOverflowCapacity> overflow{};
    /** Sandbox perks the active socket entries declare, in entry order. */
    std::array<std::uint16_t, kPerkCapacity> perks{};
};

} // namespace sunrise::state::build_data::abilities
