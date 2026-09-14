#pragma once

#include "definition.h"

namespace sunrise::state::build_data {

/** PE header timestamp (2020-08-24) and SizeOfImage of the supported game executable. */
inline constexpr std::uint32_t kSupportedImageTimestamp = 1598231435U;
inline constexpr std::uint32_t kSupportedImageSize = 145091072U;
/**
 * Join silo the supported executable selects for platform id 3 (PC). The native selector
 * returns this fixed value from a static in .data for that platform instead of deriving the
 * usual 0x5400/0x5C00 pair, so it is the silo every native join descriptor carries.
 */
inline constexpr std::uint64_t kSupportedStaticJoinSilo = 0x5C01;

/** @return false for any other executable image, so an unknown build fails closed. */
[[nodiscard]] constexpr bool static_join_silo(const BuildIdentity& build,
                                              std::uint64_t& silo) noexcept {
    silo = 0;
    if (build.imageTimestamp != kSupportedImageTimestamp || build.imageSize != kSupportedImageSize)
        return false;
    silo = kSupportedStaticJoinSilo;
    return true;
}

} // namespace sunrise::state::build_data
