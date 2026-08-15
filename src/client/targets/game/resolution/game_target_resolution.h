#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "../../../patterns/registry.h"

namespace sunrise::client::targets::game::resolution {

/** Stage that rejected the last resolve, reported without a logging dependency. */
enum class Failure : std::uint8_t {
    none,
    /** At least one signature was missing or matched more than once. */
    signature,
    /** Every signature matched but a network-group address or page check failed. */
    networkDerive,
    /** Every signature matched but a content-group address or page check failed. */
    contentDerive,
};

/**
 * Resolves every game target in one image sweep and publishes both groups together.
 * @param image Executable ranges from the main game image.
 * @return True when every signature and derived target validates.
 */
[[nodiscard]] bool resolve(std::span<const patterns::ImageRange> image) noexcept;

/** @return Stage that rejected the last resolve call. */
[[nodiscard]] Failure last_failure() noexcept;

/** @return Name of the first signature that did not match once, or an empty view. */
[[nodiscard]] std::string_view last_failed_signature() noexcept;

} // namespace sunrise::client::targets::game::resolution
