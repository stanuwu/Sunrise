#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>

#include "../../../middleware/content/packages/schema/generated/tag_classes_808094.h"

namespace sunrise::state::build_data::combat {

/**
 * Race rows. The installed race table declares 66 actor classes. A build declaring more settles
 * the taxonomy unsupported.
 */
inline constexpr std::size_t kEnemyRaceCapacity = 1024;
/** Labels one group mask can name: one bit per label. The installed table declares 306. */
inline constexpr std::size_t kLabelCapacity =
    sizeof(middleware::content::packages::schema::generated::SPerkLabelGroup::labelMask) * CHAR_BIT;
/** Group memberships across every label group; a label may sit in several groups. */
inline constexpr std::size_t kLabelGroupCapacity = 4096;

/** The race an actor class belongs to, as an ordinal of the installed race enumeration. */
struct EnemyRace {
    /** Name hash of the actor class, the hash a kill incident names its victim by. */
    std::uint32_t classHash{};
    std::uint8_t race{};
};

/** One label's membership in one named label group. */
struct LabelGroup {
    std::uint32_t groupHash{};
    std::uint32_t labelHash{};
};

} // namespace sunrise::state::build_data::combat
