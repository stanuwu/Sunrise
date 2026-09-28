#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "../../middleware/gameplay/descriptor/join_descriptor.h"

namespace sunrise::state::social {

/** The join descriptor the family-seven record declares, which is its whole body. */
inline constexpr std::size_t kNativeJoinDescriptorSize =
    middleware::gameplay::descriptor::kDescriptorSize;
/** Within it, an eight-byte fireteam hash, then this peer's own NetAddr. */
inline constexpr std::size_t kNativeJoinAddressOffset = 8;
/** And, further in, the online session id the same descriptor publishes. */
inline constexpr std::size_t kNativeJoinSessionOffset = 110;
/** The native fireteam payload struct the family-six descriptor carries behind its key. */
inline constexpr std::size_t kNativeFireteamSize = 0xB60;

/** Public fields decoded from the native character writeback, excluding inventory and unlocks. */
struct NativePresence {
    bool published{};
    /** Local selected character at the writeback's commit, not a payload-supplied owner. */
    std::uint64_t characterSoid{};
    bool hasGroup{};
    std::uint32_t groupKey{};
    std::int8_t memberCount{};
    bool hasFireteam{};
    std::array<std::byte, kNativeFireteamSize> fireteam{};
    std::uint8_t descriptorSize{};
    std::array<std::byte, kNativeJoinDescriptorSize> descriptor{};

    bool operator==(const NativePresence&) const = default;
};

/**
 * @return True when an unpublished report is entirely default; a published one additionally
 * needs consistent group flags, a descriptor within capacity with zeroed trailing bytes, and
 * a zeroed fireteam buffer unless `hasFireteam` is set.
 */
[[nodiscard]] inline bool valid(const NativePresence& value) noexcept {
    if (!value.published) {
        return value == NativePresence{};
    }
    if ((!value.hasGroup && (value.groupKey != 0 || value.memberCount != 0))
        || value.descriptorSize > value.descriptor.size()) {
        return false;
    }
    if (!value.hasFireteam
        && std::any_of(value.fireteam.begin(), value.fireteam.end(), [](std::byte byte) {
               return byte != std::byte{};
           })) {
        return false;
    }
    return std::all_of(value.descriptor.begin() + value.descriptorSize,
                       value.descriptor.end(),
                       [](std::byte byte) { return byte == std::byte{}; });
}

} // namespace sunrise::state::social
