#pragma once

#include <cstddef>
#include <span>

#include "../../../patterns/game.h"
#include "../assert_handler/game_assert_targets.h"
#include "../config_getter/game_config_getter_targets.h"
#include "../content.h"
#include "../network.h"
#include "../packages/game_package_targets.h"
#include "../retail_log.h"

namespace sunrise::client::targets::game::resolution {

/** Network signatures occupy the leading registry slice through the content-id token load. */
inline constexpr std::size_t kNetworkMatchCount =
    static_cast<std::size_t>(patterns::game::Id::contentIdTokenLoad) + 1;
/** Content signatures begin immediately after the network registry slice. */
inline constexpr std::size_t kContentFirstMatch =
    static_cast<std::size_t>(patterns::game::Id::queuezObjectResolver);
/** The aggregate match array covers the complete game signature registry. */
inline constexpr std::size_t kMatchCount = static_cast<std::size_t>(patterns::game::Id::count);
/** Retail-log signatures occupy the trailing registry slice. */
inline constexpr std::size_t kRetailLogFirstMatch =
    static_cast<std::size_t>(patterns::game::Id::retailLogEnqueue);
/** Content signatures span the registry between the network and retail-log slices. */
inline constexpr std::size_t kContentMatchCount = kRetailLogFirstMatch - kContentFirstMatch;
/** Retail-log signatures occupy the remaining registry suffix. */
inline constexpr std::size_t kRetailLogMatchCount = kMatchCount - kRetailLogFirstMatch;
// Only these signatures gate activation; the retail-log slice is diagnostic and may miss.
inline constexpr std::size_t kRequiredMatchCount = kRetailLogFirstMatch;

static_assert(kNetworkMatchCount == kContentFirstMatch);
static_assert(kContentFirstMatch + kContentMatchCount == kRetailLogFirstMatch);

} // namespace sunrise::client::targets::game::resolution

namespace sunrise::client::targets::game::network {

/**
 * Derives a network target table without publishing it.
 * @param image Executable ranges from the main game image.
 * @param matches Validated unique matches from the network registry slice.
 * @param output Receives the complete derived table.
 * @return True when every direct and derived address validates.
 */
[[nodiscard]] bool derive(std::span<const patterns::ImageRange> image,
                          std::span<const patterns::Match> matches,
                          Targets& output) noexcept;

/** @param targets Fully validated network table published without failure. */
void publish(const Targets& targets) noexcept;

} // namespace sunrise::client::targets::game::network

namespace sunrise::client::targets::game::content {

/**
 * Derives a content target table without publishing it.
 * @param image Executable ranges from the main game image.
 * @param matches Validated unique matches from the content registry slice.
 * @param output Receives the complete derived table.
 * @return True when every direct and derived address validates.
 */
[[nodiscard]] bool derive(std::span<const patterns::ImageRange> image,
                          std::span<const patterns::Match> matches,
                          Targets& output) noexcept;

/** @param targets Fully validated content table published without failure. */
void publish(const Targets& targets) noexcept;

} // namespace sunrise::client::targets::game::content

namespace sunrise::client::targets::game::retail_log {

/**
 * Derives the retail-log target table without publishing it.
 * @param matches Matches from the retail-log registry slice.
 * @param output Receives the derived table.
 * @return True when both signatures matched exactly once.
 */
[[nodiscard]] bool derive(std::span<const patterns::Match> matches, Targets& output) noexcept;

/** @param targets Fully validated retail-log table published without failure. */
void publish(const Targets& targets) noexcept;

} // namespace sunrise::client::targets::game::retail_log
