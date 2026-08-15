#include "region_reader.h"

#include <cstdio>

#include "internal.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

/** Public bubbles carry this 3-letter name prefix. */
constexpr const char* kPublicPrefix = "PUB";
/** Private bubbles carry this 3-letter name prefix. */
constexpr const char* kPrivatePrefix = "PRV";
/** Prefix, then the region index twice, split by a dot and padded to 2 digits. */
constexpr const char* kNameFormat = "%s%02u.%02u";

} // namespace

/** Formats the region name the client publishes for one region index. */
bool region_name(std::uint32_t regionIndex, bool publicBubble, RegionName& output) noexcept {
    output = {};
    if (regionIndex >= kRegionIndexBound) {
        return false;
    }
    const char* const prefix = publicBubble ? kPublicPrefix : kPrivatePrefix;
    const int written = std::snprintf(
        output.chars.data(), output.chars.size(), kNameFormat, prefix, regionIndex, regionIndex);
    if (written > 0 && static_cast<std::size_t>(written) < output.chars.size()) {
        output.length = static_cast<std::size_t>(written);
        return true;
    }
    output = {};
    return false;
}

} // namespace sunrise::middleware::content::packages::tables
