#pragma once

#include <cstdint>

namespace sunrise::state::build_data::invalidation {

/** Data Sunrise builds from package tags and can build again while the game runs; bits combine. */
enum Domain : std::uint32_t {
    kNone = 0,
    kScenarioLayouts = 1,
    kSpawnSets = 2,
    kVendors = 4,
};

/** @return The domains built from tags of this class. */
[[nodiscard]] std::uint32_t domains_for(std::uint32_t classId) noexcept;

/** How a running game takes a change to a tag class; the values are the registry's tiers. */
enum class LiveTier : std::uint8_t {
    /** Sunrise rebuilds its own data from the tag. */
    data = 1,
    /** The bytes are written into the copy the client holds. */
    patch = 2,
    /** The client reads the patched bytes at its next spawn. */
    spawn = 3,
    /** Only a restart loads the change. */
    restart = 4,
};

/** @return The registry tier of this class; a class with no rebuild here is `restart`. */
[[nodiscard]] LiveTier live_tier(std::uint32_t classId) noexcept;

/**
 * Drops the domains' published rows and the saved-cache freeze, so the next build slice reads
 * them from the packages again. Readers see no rows for a domain until its rebuild publishes.
 */
void invalidate(std::uint32_t domains) noexcept;

/** @return A count that changes when the domain is dropped; a builder that saw another restarts. */
[[nodiscard]] std::uint64_t revision(Domain domain) noexcept;

} // namespace sunrise::state::build_data::invalidation
